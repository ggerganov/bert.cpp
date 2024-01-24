#include "bert.h"
#include "ggml.h"
#include "ggml/ggml-backend.h"

#ifdef GGML_USE_CUBLAS
#include "ggml-cuda.h"
#endif

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <iostream>
#include <regex>
#include <thread>
#include <algorithm>

#define BERT_MAX_NODES 4096

// model keys

#define KEY_FTYPE "general.file_type"
#define KEY_NAME "general.name"
#define KEY_DESCRIPTION "general.description"
#define KEY_TOKEN_LIST "tokenizer.ggml.tokens"

const int verbosity = 2;

//
// utilities to get data from a gguf file
//

static int get_key_idx(const gguf_context * ctx, const char * key) {
    int i = gguf_find_key(ctx, key);
    if (i == -1) {
        fprintf(stderr, "key %s not found in file\n", key);
        throw printf("Missing required key: %s", key);
    }

    return i;
}

static uint32_t get_u32(const gguf_context * ctx, const std::string & key) {
    const int i = get_key_idx(ctx, key.c_str());

    return gguf_get_val_u32(ctx, i);
}

static float get_f32(const gguf_context * ctx, const std::string & key) {
    const int i = get_key_idx(ctx, key.c_str());

    return gguf_get_val_f32(ctx, i);
}

static std::string get_str(const gguf_context * ctx, const std::string & key) {
    const int i = get_key_idx(ctx, key.c_str());

    return gguf_get_val_str(ctx, i);
}

static struct ggml_tensor * get_tensor(struct ggml_context * ctx, const std::string & name) {
    struct ggml_tensor * cur = ggml_get_tensor(ctx, name.c_str());
    if (!cur) {
        throw printf("%s: unable to find tensor %s\n", __func__, name.c_str());
    }

    return cur;
}

static std::string get_ftype(int ftype) {
    return ggml_type_name(static_cast<ggml_type>(ftype));
}

static void tensor_stats(ggml_tensor * t) {
    printf(
        "type = %s, dims = %d, shape = (%d, %d, %d, %d)\n",
        ggml_type_name(t->type), ggml_n_dims(t), t->ne[0], t->ne[1], t->ne[2], t->ne[3]
    );
}

//
// data structures
//

// default hparams (all-MiniLM-L6-v2)
struct bert_hparams {
    int32_t n_vocab = 30522;
    int32_t n_max_tokens = 512;
    int32_t n_embd = 256;
    int32_t n_intermediate = 1536;
    int32_t n_head = 12;
    int32_t n_layer = 6;
    float_t layer_norm_eps = 1e-12;
};

struct bert_layer {
    // normalization
    struct ggml_tensor *ln_att_w;
    struct ggml_tensor *ln_att_b;

    struct ggml_tensor *ln_out_w;
    struct ggml_tensor *ln_out_b;

    // attention
    struct ggml_tensor *q_w;
    struct ggml_tensor *q_b;
    struct ggml_tensor *k_w;
    struct ggml_tensor *k_b;
    struct ggml_tensor *v_w;
    struct ggml_tensor *v_b;

    struct ggml_tensor *o_w;
    struct ggml_tensor *o_b;

    // ff
    struct ggml_tensor *ff_i_w;
    struct ggml_tensor *ff_i_b;

    struct ggml_tensor *ff_o_w;
    struct ggml_tensor *ff_o_b;
};

struct bert_vocab {
    std::map<std::string, bert_token> token_to_id;
    std::map<std::string, bert_token> subword_token_to_id;

    std::map<bert_token, std::string> _id_to_token;
    std::map<bert_token, std::string> _id_to_subword_token;
};

struct bert_model {
    bert_hparams hparams;

    // embeddings weights
    struct ggml_tensor *word_embeddings;
    struct ggml_tensor *token_type_embeddings;
    struct ggml_tensor *position_embeddings;
    struct ggml_tensor *ln_e_w;
    struct ggml_tensor *ln_e_b;

    std::vector<bert_layer> layers;
};

struct bert_ctx {
    bert_model model;
    bert_vocab vocab;

    size_t mem_per_token;
    int64_t mem_per_input;
    int32_t max_batch_n;

    struct gguf_context * ctx_gguf;
    struct ggml_context * ctx_data;

    std::vector<uint8_t> buf_compute_meta;

    // memory buffers to evaluate the model
    ggml_backend_buffer_t params_buffer = NULL;
    ggml_backend_buffer_t compute_buffer = NULL;
    ggml_backend_t backend = NULL;
    ggml_allocr * compute_alloc = NULL;
};

int32_t bert_n_embd(bert_ctx * ctx) {
    return ctx->model.hparams.n_embd;
}

int32_t bert_n_max_tokens(bert_ctx * ctx) {
    return ctx->model.hparams.n_max_tokens;
}

const char* bert_vocab_id_to_token(bert_ctx * ctx, bert_token id) {
    bert_vocab & vocab = ctx->vocab;
    auto it = vocab._id_to_token.find(id);
    if (it != vocab._id_to_token.end()) {
        return it->second.c_str();
    }
    it = vocab._id_to_subword_token.find(id);
    if (it != vocab._id_to_subword_token.end()) {
        return it->second.c_str();
    }
    return "[UNK TOKEN from bert_vocab]";
}

//
// command line interface
//

void bert_print_usage(char **argv, const bert_params &params) {
    fprintf(stderr, "usage: %s [options]\n", argv[0]);
    fprintf(stderr, "\n");
    fprintf(stderr, "options:\n");
    fprintf(stderr, "  -h, --help            show this help message and exit\n");
    fprintf(stderr, "  -s SEED, --seed SEED  RNG seed (default: -1)\n");
    fprintf(stderr, "  -t N, --threads N     number of threads to use during computation (default: %d)\n", params.n_threads);
    fprintf(stderr, "  -p PROMPT, --prompt PROMPT\n");
    fprintf(stderr, "                        prompt to start generation with (default: random)\n");
    fprintf(stderr, "  -m FNAME, --model FNAME\n");
    fprintf(stderr, "                        model path (default: %s)\n", params.model);
    fprintf(stderr, "\n");
}

bool bert_params_parse(int argc, char **argv, bert_params &params) {
    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];

        if (arg == "-t" || arg == "--threads") {
            params.n_threads = std::stoi(argv[++i]);
        } else if (arg == "-p" || arg == "--prompt") {
            params.prompt = argv[++i];
        } else if (arg == "-m" || arg == "--model") {
            params.model = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            bert_print_usage(argv, params);
            exit(0);
        } else {
            fprintf(stderr, "error: unknown argument: %s\n", arg.c_str());
            bert_print_usage(argv, params);
            exit(0);
        }
    }

    return true;
}

//
// tokenizing
//

static size_t utf8_len(char src) {
    const size_t lookup[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 3, 4};
    uint8_t highbits = static_cast<uint8_t>(src) >> 4;
    return lookup[highbits];
}

std::string strip_accents(const std::string &inputString) {
    std::string resultString;
    std::map<std::string, char> accentMap = {{"À", 'A'},{"Á", 'A'},
        {"Â", 'A'},{"Ã", 'A'},{"Ä", 'A'},{"Å", 'A'},{"à", 'a'},{"á", 'a'},
        {"â", 'a'},{"ã", 'a'},{"ä", 'a'},{"å", 'a'},{"È", 'E'},{"É", 'E'},
        {"Ê", 'E'},{"Ë", 'E'},{"è", 'e'},{"é", 'e'},{"ê", 'e'},{"ë", 'e'},
        {"Ì", 'I'},{"Í", 'I'},{"Î", 'I'},{"Ï", 'I'},{"ì", 'i'},{"í", 'i'},
        {"î", 'i'},{"ï", 'i'},{"Ò", 'O'},{"Ó", 'O'},{"Ô", 'O'},{"Õ", 'O'},
        {"Ö", 'O'},{"ò", 'o'},{"ó", 'o'},{"ô", 'o'},{"õ", 'o'},{"ö", 'o'},
        {"Ù", 'U'},{"Ú", 'U'},{"Û", 'U'},{"Ü", 'U'},{"ù", 'u'},{"ú", 'u'},
        {"û", 'u'},{"ü", 'u'},{"Ý", 'Y'},{"ý", 'y'},{"Ç", 'C'},{"ç", 'c'},
        {"Ñ", 'N'},{"ñ", 'n'},
    };

    for (size_t i = 0; i < inputString.length();)
    {
        int len = utf8_len(inputString[i]);
        std::string curChar = inputString.substr(i, len);
        auto iter = accentMap.find(curChar);
        if (iter != accentMap.end())
        {
            resultString += iter->second;
        }
        else
        {
            resultString += curChar;
        }
        i += len;
    }

    return resultString;
}

std::string bert_normalize_prompt(const std::string &text)
{
    // TODO: handle chinese characters? https://github.com/huggingface/tokenizers/blob/ef5f50605ddf9f8caef1598c0e4853862b9707a7/tokenizers/src/normalizers/bert.rs#L98
    std::string text2 = strip_accents(text);
    for (size_t i = 0; i < text2.size(); i += utf8_len(text2[i]))
    {
        char c = text2[i];
        if (c >= 'A' && c <= 'Z')
            text2[i] = c - 'A' + 'a';
    }
    return text2;
}

bool is_chinese_char(const std::string& str) {
    int len = str.length();
    unsigned int codepoint = 0;
    int num_bytes = 0;
    int i = 0;
    unsigned char ch = static_cast<unsigned char>(str[i]);
    if (ch <= 0x7f) {
        codepoint = ch;
        num_bytes = 1;
    } else if ((ch >> 5) == 0x06) {
        codepoint = ch & 0x1f;
        num_bytes = 2;
    } else if ((ch >> 4) == 0x0e) {
        codepoint = ch & 0x0f;
        num_bytes = 3;
    } else if ((ch >> 3) == 0x1e) {
        codepoint = ch & 0x07;
        num_bytes = 4;
    }
    for (int j = 1; j < num_bytes; ++j) {
        if (i + j >= len) {
            return false; // incomplete UTF-8 character
        }
        unsigned char next_ch = static_cast<unsigned char>(str[i + j]);
        if ((next_ch >> 6) != 0x02) {
            return false; // invalid trailing byte
        }
        codepoint = (codepoint << 6) | (next_ch & 0x3f);
    }
    if ((codepoint >= 0x4E00 && codepoint <= 0x9FFF) ||
        (codepoint >= 0x3400 && codepoint <= 0x4DBF) ||
        (codepoint >= 0x20000 && codepoint <= 0x2A6DF) ||
        (codepoint >= 0x2A700 && codepoint <= 0x2B73F) ||
        (codepoint >= 0x2B740 && codepoint <= 0x2B81F) ||
        (codepoint >= 0x2B920 && codepoint <= 0x2CEAF) || // this should be 0x2B820 but in hf rust code it is 0x2B920
        (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||
        (codepoint >= 0x2F800 && codepoint <= 0x2FA1F) ||
        (codepoint >= 0x3000 && codepoint <= 0x303F) ||
        (codepoint >= 0xFF00 && codepoint <= 0xFFEF)) {
        return true;
    }
    return false;
}

bert_tokens bert_tokenize(struct bert_ctx * ctx, bert_string text, int32_t n_max_tokens) {
    int cls_tok_id = 101;
    int sep_tok_id = 102;
    int unk_tok_id = 100;
    const bert_vocab &vocab = ctx->vocab;

    std::string ori_str = text;
    ori_str = bert_normalize_prompt(ori_str);

    // single punct / single symbol / single digit
    // baseline: add whitespace on the left and right of punct and chinese characters
    std::vector<std::string> words;
    std::string new_str = "";
    int i = 0;
    while (i < ori_str.size()) {
        int utf_char_len = utf8_len(ori_str[i]);
        if ((utf_char_len == 1) && ispunct(ori_str[i])) {
            new_str += " ";
            new_str += ori_str[i];
            new_str += " ";
            i += 1;
        }
        else if ((utf_char_len == 3) && is_chinese_char(ori_str.substr(i, 3))) {
            new_str += " ";
            new_str += ori_str.substr(i, 3);
            new_str += " ";
            i += 3;
        }
        else {
            new_str += ori_str[i];
            i += 1;
        }
    }

    int l = 0;
    int r = 0;
    while (r < new_str.size()) {
        // if is whitespace
        if (isspace(new_str[r])) {
            if (r > l)
                words.push_back(new_str.substr(l, (r - l)));
            l = r + 1;
            r = l;
        }
        else {
            r += 1;
        }
    }
    if (r > l) {
        words.push_back(new_str.substr(l, (r - l)));
    }

    int32_t t = 0;
    int32_t prev_t = 0;

    bert_tokens tokens;
    tokens.push_back(cls_tok_id);
    t++;

    // find the longest tokens that form the words:
    for (const auto &word : words) {
        int n = word.size();
        if (n == 0) {
            continue;
        }
        prev_t = t;

        int i = 0;
        auto * token_map = &vocab.token_to_id;
    loop:
        while (i < n) {
            if (t >= n_max_tokens - 1) {
                break;
            }
            int j = n;
            while (j > i) {
                auto it = token_map->find(word.substr(i, j - i));
                if (it != token_map->end()) {
                    tokens.push_back(it->second);
                    t++;
                    i = j;
                    token_map = &vocab.subword_token_to_id;
                    goto loop;
                }
                --j;
            }
            if (j == i) {
                fprintf(stderr, "%s: unknown token '%s'\n", __func__, word.substr(i, 1).data());
                token_map = &vocab.subword_token_to_id;
                ++i;
            }
        }
        if (prev_t == t) {
            // fprintf(stderr, "%s: unknown token '%s'\n", __func__, word.data());
            tokens.push_back(unk_tok_id);
            t++;
        }

    }
    tokens.push_back(sep_tok_id);
    t++;

    // return tokens
    return tokens;
}

//
// loading and setup
//

struct bert_ctx * bert_load_from_file(const char *fname) {
    printf("%s: loading model from '%s' - please wait ...\n", __func__, fname);

    struct ggml_context * meta = NULL;

    struct gguf_init_params params = {
        /*.no_alloc = */ true,
        /*.ctx      = */ &meta,
    };

    struct gguf_context * ctx = gguf_init_from_file(fname, params);
    if (!ctx) {
        throw printf("%s: failed to load BERT model from %s. Does this file exist?\n", __func__, fname);
    }

    if (verbosity >= 1) {
        const int n_tensors = gguf_get_n_tensors(ctx);
        const int n_kv = gguf_get_n_kv(ctx);
        const int ftype = get_u32(ctx, KEY_FTYPE);
        const int alignment = gguf_get_alignment(ctx);
        const int version = gguf_get_version(ctx);
        const std::string ftype_str = get_ftype(ftype);
        const std::string description = get_str(ctx, KEY_DESCRIPTION);
        const std::string name = get_str(ctx, KEY_NAME);
        printf("%s: model name:   %s\n", __func__, name.c_str());
        printf("%s: description:  %s\n", __func__, description.c_str());
        printf("%s: GGUF version: %d\n", __func__, version);
        printf("%s: alignment:    %zu\n", __func__, alignment);
        printf("%s: n_tensors:    %d\n", __func__, n_tensors);
        printf("%s: n_kv:         %d\n", __func__, n_kv);
        printf("%s: ftype:        %s\n", __func__, ftype_str.c_str());
        printf("\n");
    }
    const int n_tensors = gguf_get_n_tensors(ctx);

    // kv
    if (verbosity >= 3) {
        const int n_kv = gguf_get_n_kv(ctx);

        for (int i = 0; i < n_kv; ++i) {
            const char * key = gguf_get_key(ctx, i);

            printf("%s: kv[%d]: key = %s\n", __func__, i, key);
        }
        printf("\n");
    }

    // create model object
    bert_ctx * new_bert = new bert_ctx;
    bert_model & model = new_bert->model;
    bert_vocab & vocab = new_bert->vocab;
    bert_hparams & hparams = model.hparams;

    // load hparams (FIXME)
    {
        hparams.n_vocab = get_u32(ctx, "vocab_size");
        hparams.n_max_tokens = get_u32(ctx, "max_position_embedding");
        hparams.n_embd = get_u32(ctx, "hidden_size");
        hparams.n_intermediate = get_u32(ctx, "intermediate_size");
        hparams.n_head = get_u32(ctx, "num_attention_heads");
        hparams.n_layer = get_u32(ctx, "num_hidden_layers");
        hparams.layer_norm_eps = get_f32(ctx, "layer_norm_eps");

        printf("%s: n_vocab        = %d\n", __func__, hparams.n_vocab);
        printf("%s: n_max_tokens   = %d\n", __func__, hparams.n_max_tokens);
        printf("%s: n_embd         = %d\n", __func__, hparams.n_embd);
        printf("%s: n_intermediate = %d\n", __func__, hparams.n_intermediate);
        printf("%s: n_head         = %d\n", __func__, hparams.n_head);
        printf("%s: n_layer        = %d\n", __func__, hparams.n_layer);
        printf("%s: layer_norm_eps = %g\n", __func__, hparams.layer_norm_eps);
        printf("\n");
    }

    // load vocab
    {
        const int token_idx = gguf_find_key(ctx, KEY_TOKEN_LIST);
        const int n_vocab = gguf_get_arr_n(ctx, token_idx);

        for (int i = 0; i < n_vocab; i++) {
            std::string word = gguf_get_arr_str(ctx, token_idx, i);

            if (word[0] == '#' && word[1] == '#') {
                vocab.subword_token_to_id[word.substr(2)] = i;
                vocab._id_to_subword_token[i] = word;
            }

            if (vocab.token_to_id.count(word) == 0) {
                vocab.token_to_id[word] = i;
                vocab._id_to_token[i] = word;
            }
        }
    }

    // data
    size_t buffer_size = 0;
    {
        for (int i = 0; i < n_tensors; ++i) {
            const char * name = gguf_get_tensor_name(ctx, i);
            const size_t offset = gguf_get_tensor_offset(ctx, i);
            struct ggml_tensor * cur = ggml_get_tensor(meta, name);
            size_t tensor_size = ggml_nbytes(cur);
            buffer_size += tensor_size;
            if (verbosity >= 3) {
                printf("%s: tensor[%d]: type = %s, n_dims = %d, name = %s, offset=%zu, type=%d\n", __func__, i,
                       ggml_type_name(cur->type), ggml_n_dims(cur), cur->name, offset, cur->type);
            }
        }
    }

    // initialize backend
#ifdef GGML_USE_CUBLAS
    new_bert->backend = ggml_backend_cuda_init(0);
    printf("%s: BERT using CUDA backend: %p\n", __func__, new_bert->backend);
#endif

#ifdef GGML_USE_METAL
    new_bert->backend = ggml_backend_metal_init();
    printf("%s: BERT using Metal backend: %p\n", __func__, new_bert->backend);
#endif

    if (!new_bert->backend) {
        new_bert->backend = ggml_backend_cpu_init();
        printf("%s: BERT using CPU backend\n", __func__);
    }

    // load tensors
    {
        std::vector<uint8_t> read_buf;
        struct ggml_init_params params = {
            /*.mem_size =*/ (n_tensors + 1) * ggml_tensor_overhead(),
            /*.mem_buffer =*/ NULL,
            /*.no_alloc =*/ true,
        };

        new_bert->ctx_data = ggml_init(params);
        if (!new_bert->ctx_data) {
            fprintf(stderr, "%s: ggml_init() failed\n", __func__);
            free(new_bert);
            return nullptr;
        }

        auto fin = std::ifstream(fname, std::ios::binary);
        if (!fin) {
            printf("cannot open model file for loading tensors\n");
            free(new_bert);
            return nullptr;
        }

        // add tensors to context
        for (int i = 0; i < n_tensors; ++i) {
            const char * name = gguf_get_tensor_name(ctx, i);
            struct ggml_tensor * t = ggml_get_tensor(meta, name);
            struct ggml_tensor * cur = ggml_dup_tensor(new_bert->ctx_data, t);
            ggml_set_name(cur, name);
        }

        // alloc memory and offload data
        new_bert->params_buffer = ggml_backend_alloc_buffer(new_bert->backend, buffer_size);
        ggml_allocr* alloc = ggml_allocr_new_from_buffer(new_bert->params_buffer);
        for (int i = 0; i < n_tensors; ++i) {
            const char * name = gguf_get_tensor_name(ctx, i);
            struct ggml_tensor * cur = ggml_get_tensor(new_bert->ctx_data, name);
            ggml_allocr_alloc(alloc, cur);
            const size_t offset = gguf_get_data_offset(ctx) + gguf_get_tensor_offset(ctx, i);
            fin.seekg(offset, std::ios::beg);
            if (!fin) {
                printf("%s: failed to seek for tensor %s\n", __func__, name);
                bert_free(new_bert);
                return nullptr;
            }
            int num_bytes = ggml_nbytes(cur);
            if (ggml_backend_buffer_is_host(new_bert->params_buffer)) {
                // for the CPU and Metal backend, we can read directly into the tensor
                fin.read(reinterpret_cast<char *>(cur->data), num_bytes);
            } else {
                // read into a temporary buffer first, then copy to device memory
                read_buf.resize(num_bytes);
                fin.read(reinterpret_cast<char *>(read_buf.data()), num_bytes);
                ggml_backend_tensor_set(cur, read_buf.data(), 0, num_bytes);
            }
        }
        ggml_allocr_free(alloc);
    }

    // use get_tensors to populate bert_model
    {
        // embeddings weights
        model.word_embeddings = get_tensor(new_bert->ctx_data, "embeddings.word_embeddings.weight");
        model.token_type_embeddings = get_tensor(new_bert->ctx_data, "embeddings.token_type_embeddings.weight");
        model.position_embeddings = get_tensor(new_bert->ctx_data, "embeddings.position_embeddings.weight");
        model.ln_e_w = get_tensor(new_bert->ctx_data, "embeddings.LayerNorm.weight");
        model.ln_e_b = get_tensor(new_bert->ctx_data, "embeddings.LayerNorm.bias");

        // layers
        model.layers.resize(hparams.n_layer);
        for (int i = 0; i < hparams.n_layer; ++i) {
            bert_layer & layer = model.layers[i];
            std::string pre = "encoder.layer." + std::to_string(i) + ".";

            // normalization
            layer.ln_att_w = get_tensor(new_bert->ctx_data, pre + "attention.output.LayerNorm.weight");
            layer.ln_att_b = get_tensor(new_bert->ctx_data, pre + "attention.output.LayerNorm.bias");

            layer.ln_out_w = get_tensor(new_bert->ctx_data, pre + "output.LayerNorm.weight");
            layer.ln_out_b = get_tensor(new_bert->ctx_data, pre + "output.LayerNorm.bias");

            // attention
            layer.q_w = get_tensor(new_bert->ctx_data, pre + "attention.self.query.weight");
            layer.q_b = get_tensor(new_bert->ctx_data, pre + "attention.self.query.bias");
            layer.k_w = get_tensor(new_bert->ctx_data, pre + "attention.self.key.weight");
            layer.k_b = get_tensor(new_bert->ctx_data, pre + "attention.self.key.bias");
            layer.v_w = get_tensor(new_bert->ctx_data, pre + "attention.self.value.weight");
            layer.v_b = get_tensor(new_bert->ctx_data, pre + "attention.self.value.bias");

            layer.o_w = get_tensor(new_bert->ctx_data, pre + "attention.output.dense.weight");
            layer.o_b = get_tensor(new_bert->ctx_data, pre + "attention.output.dense.bias");

            // ff
            layer.ff_i_w = get_tensor(new_bert->ctx_data, pre + "intermediate.dense.weight");
            layer.ff_i_b = get_tensor(new_bert->ctx_data, pre + "intermediate.dense.bias");

            layer.ff_o_w = get_tensor(new_bert->ctx_data, pre + "output.dense.weight");
            layer.ff_o_b = get_tensor(new_bert->ctx_data, pre + "output.dense.bias");
        }
    }

    // free metadata
    ggml_free(meta);
    new_bert->ctx_gguf = ctx;

    // measure mem requirement and allocate
    {
        // allocate space for graph
        new_bert->buf_compute_meta.resize(GGML_DEFAULT_GRAPH_SIZE * ggml_tensor_overhead() + ggml_graph_overhead());
        new_bert->compute_alloc = ggml_allocr_new_measure_from_backend(new_bert->backend);

        // construct batch and compute graph
        bert_tokens tokens(hparams.n_max_tokens);
        bert_batch batch;
        batch.push_back(tokens);
        ggml_cgraph * gf = bert_build_graph(new_bert, batch);

        size_t compute_memory_buffer_size = ggml_allocr_alloc_graph(new_bert->compute_alloc, gf);
        ggml_allocr_free(new_bert->compute_alloc);
        new_bert->compute_buffer = ggml_backend_alloc_buffer(new_bert->backend, compute_memory_buffer_size);
        new_bert->compute_alloc = ggml_allocr_new_from_buffer(new_bert->compute_buffer);

        printf("%s: compute allocated memory: %.2f MB\n", __func__, compute_memory_buffer_size / 1024.0 / 1024.0);
    }

    return new_bert;
}

void bert_free(bert_ctx * ctx) {
    ggml_free(ctx->ctx_data);
    gguf_free(ctx->ctx_gguf);

    delete ctx;
}

//
// model execution
//

ggml_cgraph * bert_build_graph(bert_ctx * ctx, bert_batch batch) {
    const bert_model & model = ctx->model;
    const bert_hparams & hparams = model.hparams;

    // get the max length of the batch
    int n_batch_size = batch.size();
    int cur_max_len = 0;
    for (int ba = 0; ba < batch.size(); ba++) {
        int n = batch[ba].size();
        if (n > cur_max_len)
            cur_max_len = n;
    }

    // extract model params
    const int n_embd = hparams.n_embd;
    const int n_layer = hparams.n_layer;
    const int n_max_tokens = hparams.n_max_tokens;
    const int n_head = hparams.n_head;
    const float layer_norm_eps = hparams.layer_norm_eps;
    const int d_head = n_embd / n_head; // E = D * H

    // check for token overflow
    if (cur_max_len > n_max_tokens) {
        fprintf(stderr, "Too many tokens, maximum is %d\n", n_max_tokens);
        return nullptr;
    }

    // params for graph data
    struct ggml_init_params params = {
        /*.mem_size   =*/ ctx->buf_compute_meta.size(),
        /*.mem_buffer =*/ ctx->buf_compute_meta.data(),
        /*.no_alloc   =*/ true,
    };

    // initialze computational graph
    struct ggml_context * ctx0 = ggml_init(params);
    struct ggml_cgraph * gf = ggml_new_graph_custom(ctx0, BERT_MAX_NODES, false);

    // embeddings = word_embeddings + token_type_embeddings + position_embeddings
    struct ggml_tensor *token_layer = ggml_new_tensor_1d(ctx0, GGML_TYPE_I32, cur_max_len * n_batch_size);
    struct ggml_tensor *pad_mask = ggml_new_tensor_4d(ctx0, GGML_TYPE_F32, 1, cur_max_len, 1, n_batch_size);
    struct ggml_tensor *positions = ggml_new_tensor_1d(ctx0, GGML_TYPE_I32, cur_max_len * n_batch_size);
    struct ggml_tensor *sum = ggml_new_tensor_3d(ctx0, GGML_TYPE_F32, cur_max_len, 1, n_batch_size); // the avg pooler
    ggml_allocr_alloc(ctx->compute_alloc, token_layer);
    ggml_allocr_alloc(ctx->compute_alloc, pad_mask);
    ggml_allocr_alloc(ctx->compute_alloc, positions);
    ggml_allocr_alloc(ctx->compute_alloc, sum);

    // avoid writing input embeddings in memory measure mode
    if (!ggml_allocr_is_measure(ctx->compute_alloc)) {
        int32_t * token_layer_data = (int32_t*)malloc(ggml_nbytes(token_layer));
        float * pad_mask_data = (float*)malloc(ggml_nbytes(pad_mask));
        int32_t * pos_data = (int32_t*)malloc(ggml_nbytes(positions));
        float * sum_data = (float*)malloc(ggml_nbytes(sum));

        for (int ba = 0; ba < n_batch_size; ba++) {
            for (int i = 0; i < cur_max_len; i++) {
                int cur_len = batch[ba].size();
                if (i < cur_len) {
                    token_layer_data[ba * cur_max_len + i] = batch[ba][i];
                    pad_mask_data[ba * cur_max_len + i] = 1.0f;
                    sum_data[ba * cur_max_len + i] = 1 / (float)cur_len;
                }
                else {
                    token_layer_data[ba * cur_max_len + i] = 101; // padding
                    pad_mask_data[ba * cur_max_len + i] = 0.0f;
                    sum_data[ba * cur_max_len + i] = 0.0f;
                }
                pos_data[ba * cur_max_len + i] = i;
            }
        }

        ggml_backend_tensor_set(token_layer, token_layer_data, 0, ggml_nbytes(token_layer));
        ggml_backend_tensor_set(pad_mask, pad_mask_data, 0, ggml_nbytes(pad_mask));
        ggml_backend_tensor_set(positions, pos_data, 0, ggml_nbytes(positions));
        ggml_backend_tensor_set(sum, sum_data, 0, ggml_nbytes(sum));

        free(token_layer_data);
        free(pad_mask_data);
        free(pos_data);
        free(sum_data);
    }

    // outer product the padding mask to kill off outside
    struct ggml_tensor * attn_mask = ggml_mul_mat(ctx0, pad_mask, pad_mask); // [L, L, 1, B]
    attn_mask = ggml_add1(ctx0, attn_mask, ggml_new_f32(ctx0, -1.0f)); // result -0
    attn_mask = ggml_scale(ctx0, attn_mask, 100000.0f); // BUG: 1e3 will cause overflow?

    // broadcast LxL attention to each head and batch element
    attn_mask = ggml_repeat(ctx0, attn_mask, ggml_new_tensor_4d(ctx0, GGML_TYPE_I32, cur_max_len, cur_max_len, n_head, n_batch_size)); // [L, L, H, B]
    attn_mask = ggml_reshape_3d(ctx0, attn_mask, cur_max_len, cur_max_len, n_head * n_batch_size); // [L, L, H * B]

    // set up token types (always zero?)
    struct ggml_tensor *token_types = ggml_new_tensor_1d(ctx0, GGML_TYPE_I32, cur_max_len * n_batch_size);
    ggml_allocr_alloc(ctx->compute_alloc, token_types);
    if (!ggml_allocr_is_measure(ctx->compute_alloc)) {
        ggml_set_zero(token_types);
    }

    // get various embedding components
    struct ggml_tensor *inpL = ggml_get_rows(ctx0, model.word_embeddings, token_layer); // [E, L * B]
    inpL = ggml_add(ctx0, ggml_get_rows(ctx0, model.token_type_embeddings, token_types), inpL);
    inpL = ggml_add(ctx0, ggml_get_rows(ctx0, model.position_embeddings, positions), inpL);
    inpL = ggml_reshape_3d(ctx0, inpL, n_embd, cur_max_len, n_batch_size); // [E, L, B]

    // embed layern norm
    inpL = ggml_norm_inplace(ctx0, inpL, layer_norm_eps);
    inpL = ggml_add(ctx0, ggml_mul(ctx0, inpL, model.ln_e_w), model.ln_e_b); // [E, L, B]

    // layers
    for (int il = 0; il < n_layer; il++) {
        struct ggml_tensor * cur = inpL;

        // self-attention
        {
            // extract Q
            struct ggml_tensor * Qcur = cur;
            Qcur = ggml_add(ctx0, ggml_mul_mat(ctx0, model.layers[il].q_w, Qcur), model.layers[il].q_b); // [E, L, B]
            Qcur = ggml_reshape_4d(ctx0, Qcur, d_head, n_head, cur_max_len, n_batch_size); // [D, H, L, B]
            struct ggml_tensor * Q = ggml_cont(ctx0, ggml_permute(ctx0, Qcur, 0, 2, 1, 3)); // [D, L, H, B]
            Q = ggml_reshape_3d(ctx0, Q, d_head, cur_max_len, n_head * n_batch_size); // [D, L, H * B]

            // extract K
            struct ggml_tensor * Kcur = cur;
            Kcur = ggml_add(ctx0, ggml_mul_mat(ctx0, model.layers[il].k_w, Kcur), model.layers[il].k_b); // [E, L, B]
            Kcur = ggml_reshape_4d(ctx0, Kcur, d_head, n_head, cur_max_len, n_batch_size); // [D, H, L, B]
            struct ggml_tensor * K = ggml_cont(ctx0, ggml_permute(ctx0, Kcur, 0, 2, 1, 3)); // [D, L, H, B]
            K = ggml_reshape_3d(ctx0, K, d_head, cur_max_len, n_head * n_batch_size); // [D, L, H * B]

            // extract V
            struct ggml_tensor * Vcur = cur;
            Vcur = ggml_add(ctx0, ggml_mul_mat(ctx0, model.layers[il].v_w, Vcur), model.layers[il].v_b); // [E, L, B]
            Vcur = ggml_reshape_4d(ctx0, Vcur, d_head, n_head, cur_max_len, n_batch_size); // [D, H, L, B]
            struct ggml_tensor * V = ggml_cont(ctx0, ggml_permute(ctx0, Vcur, 0, 2, 1, 3)); // [D, L, H, B]
            V = ggml_reshape_3d(ctx0, V, d_head, cur_max_len, n_head * n_batch_size); // [D, L, H * B]

            // scaled attention
            struct ggml_tensor * KQ = ggml_mul_mat(ctx0, K, Q); // -> [L, L, H * B]
            KQ = ggml_scale(ctx0, KQ, 1.0f / sqrt((float)d_head));
            KQ = ggml_add(ctx0, KQ, attn_mask);
            KQ = ggml_soft_max(ctx0, KQ);

            V = ggml_cont(ctx0, ggml_transpose(ctx0, V)); // -> [L, D, H * B]
            struct ggml_tensor * KQV = ggml_mul_mat(ctx0, V, KQ); // -> [D, L, H * B]
            KQV = ggml_reshape_4d(ctx0, KQV, d_head, cur_max_len, n_head, n_batch_size); // -> [D, L, H, B]
            KQV = ggml_cont(ctx0, ggml_permute(ctx0, KQV, 0, 2, 1, 3)); // -> [D, H, L, B]

            // copy back to input (E = D * H)
            cur = ggml_cpy(ctx0, KQV, ggml_new_tensor_3d(ctx0, GGML_TYPE_F32, n_embd, cur_max_len, n_batch_size)); // [E, L, B]
        }

        // attention output
        cur = ggml_add(ctx0, ggml_mul_mat(ctx0, model.layers[il].o_w, cur), model.layers[il].o_b);

        // residual connection
        cur = ggml_add(ctx0, cur, inpL);

        // attention layer norm
        cur = ggml_norm_inplace(ctx0, cur, layer_norm_eps);
        cur = ggml_add(ctx0, ggml_mul(ctx0, cur, model.layers[il].ln_att_w), model.layers[il].ln_att_b);

        // store for later
        struct ggml_tensor * att_output = cur;

        // feed forward steps
        cur = ggml_add(ctx0, ggml_mul_mat(ctx0, model.layers[il].ff_i_w, cur), model.layers[il].ff_i_b);
        cur = ggml_gelu(ctx0, cur);
        cur = ggml_add(ctx0, ggml_mul_mat(ctx0, model.layers[il].ff_o_w, cur), model.layers[il].ff_o_b);

        // attentions bypass the intermediate layer
        cur = ggml_add(ctx0, att_output, cur);

        // output layer norm
        cur = ggml_norm_inplace(ctx0, cur, layer_norm_eps);
        cur = ggml_add(ctx0, ggml_mul(ctx0, cur, model.layers[il].ln_out_w), model.layers[il].ln_out_b);

        // on to next layer
        inpL = cur;
    }

    // pooler (sum = [L, 1, B])
    inpL = ggml_mul_mat(ctx0, ggml_cont(ctx0, ggml_transpose(ctx0, inpL)), sum); // [E, 1, B]

    // normalizer
    ggml_tensor * length = ggml_sqrt(ctx0, ggml_sum_rows(ctx0, ggml_sqr(ctx0, inpL))); // [1, 1, B]
    inpL = ggml_div(ctx0, inpL, ggml_repeat(ctx0, length, inpL)); // [E, 1, B]
    inpL = ggml_reshape_2d(ctx0, inpL, n_embd, n_batch_size); // [E, B]

    // final output
    ggml_tensor * output = inpL;

    // build the graph
    ggml_build_forward_expand(gf, output);

    // free context
    ggml_free(ctx0);

    // return complete graph
    return gf;
}

void bert_forward_batch(bert_ctx * ctx, bert_batch batch, float * embeddings, int32_t n_threads) {
    // reset alloc buffer to clean the memory from previous invocations
    ggml_allocr_reset(ctx->compute_alloc);

    // build the inference graph
    ggml_cgraph * gf = bert_build_graph(ctx, batch);
    ggml_allocr_alloc_graph(ctx->compute_alloc, gf);

    // print timing information per ggml operation (for debugging purposes)
    if (verbosity >= 3) {
        ggml_graph_print(gf);
    }

    if (ggml_backend_is_cpu(ctx->backend)) {
        ggml_backend_cpu_set_n_threads(ctx->backend, n_threads);
    }

#ifdef GGML_USE_METAL
    if (ggml_backend_is_metal(ctx->backend)) {
        ggml_backend_metal_set_n_cb(ctx->backend, n_threads);
    }
#endif

    ggml_backend_graph_compute(ctx->backend, gf);

    // the last node is the embedding tensor
    struct ggml_tensor * output = gf->nodes[gf->n_nodes - 1];
    tensor_stats(output);

    // copy the embeddings to the location passed by the user
    ggml_backend_tensor_get(output, embeddings, 0, ggml_nbytes(output));
}

void bert_encode_batch(struct bert_ctx * ctx, bert_strings texts, float * embeddings, int32_t n_threads) {
    int32_t N = bert_n_max_tokens(ctx);
    int32_t n_inputs = texts.size();

    bert_batch batch;
    for (int i = 0; i < n_inputs; i++) {
        bert_tokens tokens = bert_tokenize(ctx, texts[i], N);
        batch.push_back(tokens);
    }

    bert_forward_batch(ctx, batch, embeddings, n_threads);
}

void bert_forward(struct bert_ctx * ctx, bert_tokens tokens, float * embeddings, int32_t n_threads) {
    bert_batch batch = {tokens};
    bert_forward_batch(ctx, batch, embeddings, n_threads);
}

void bert_encode(struct bert_ctx * ctx, bert_string text, float * embeddings, int32_t n_threads) {
    bert_strings strings = {text};
    bert_encode_batch(ctx, strings, embeddings, n_threads);
}
