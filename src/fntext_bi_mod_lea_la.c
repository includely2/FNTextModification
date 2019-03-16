#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <string.h>
#include <assert.h>
#include <omp.h>

#define EM_RANGE (0.01)
#define LAMBDA (0.1)  // weight of postional embedding look up table

struct model_t
{
    float *em, *w, *b;
    float *em_bi, *w_bi;
    float *em_pos, *em_bi_pos;
    float *w_positional; // original + positional embedding
    float *w_bi_positional;  // bi-gram + positional embedding
    int64_t em_dim, vocab_num, category_num;
};

struct dataset_t
{
    int64_t *text_indices, *text_lens, *text_categories;
    int64_t *start_pos;
    int64_t text_num;  // number of word-sequences (line)
};

void init_model(struct model_t *model, int64_t em_dim, int64_t vocab_num, int64_t category_num, int64_t max_text_len, int64_t is_init)
{
    model->em_dim = em_dim;
    model->vocab_num = vocab_num;
    model->category_num = category_num;

    model->em = (float *)malloc(em_dim * vocab_num * sizeof(float));  // look up table for original
    model->em_pos = (float *)malloc(em_dim * max_text_len * sizeof(float));  // look up table for positional embedding 
    model->em_bi = (float *)malloc(em_dim * vocab_num * sizeof(float));  // look up table for bi-gram
    model->em_bi_pos = (float *)malloc(em_dim * max_text_len * sizeof(float));  // look up table for bi positional embedding
    model->w = (float *)malloc(em_dim * category_num * sizeof(float)); // FC weight original
    model->w_bi = (float *)malloc(em_dim * category_num * sizeof(float)); // FC weight bi-gram
    model->w_positional = (float *)malloc(em_dim * category_num * sizeof(float));  // FC weight positional embedding
    model->w_bi_positional = (float *)malloc(em_dim * category_num * sizeof(float));  // FC weight bi positional embedding

    model->b = (float *)malloc(category_num * sizeof(float));  // FC bias

    float *em = model->em;
    float *em_pos = model->em_pos;
    float *em_bi = model->em_bi;
    float *em_bi_pos = model->em_bi_pos;
    float *w = model->w;
    float *w_bi = model->w_bi;
    float *w_positional = model->w_positional;
    float *w_bi_positional = model->w_bi_positional;
    float *b = model->b;

    int64_t i, j;
    if (is_init)
    {
        srand(time(NULL));
        // uniform distribution for look up table [-EM_RANGE, EM_RANGE]
        for (i = 0; i < em_dim * vocab_num; i++)
        {
            em[i] = ((float)rand() / RAND_MAX) * 2. * EM_RANGE - EM_RANGE;  // randomly initialize original table
            em_bi[i] = ((float)rand() / RAND_MAX) * 2. * EM_RANGE - EM_RANGE;  // randomly initialize bi-gram table
        }
        for (i = 0; i < max_text_len; i++)  // initialize with fixed positional embedding
        {
            for (j = 0; j < em_dim; j++)
            {
                if (j % 2 == 0)
                {
                    em_pos[i * em_dim + j] = EM_RANGE * sin(i/pow(max_text_len, j/em_dim));
                    em_bi_pos[i * em_dim + j] = EM_RANGE * sin(i/pow(max_text_len, j/em_dim));
                }
                else
                {
                    em_pos[i * em_dim + j] = EM_RANGE * cos(i/pow(max_text_len, (j-1)/em_dim));
                    em_bi_pos[i * em_dim + j] = EM_RANGE * cos(i/pow(max_text_len, (j-1)/em_dim));
                }
            }
            //em_pos[i] = ((float)rand() / RAND_MAX) * 2. * EM_RANGE - EM_RANGE;  // randomly initialize positional embedding table
            //em_bi_pos[i] = ((float)rand() / RAND_MAX) * 2. * EM_RANGE - EM_RANGE;  // randomly initialize bi positional embedding table
        }
        // uniform distribution for weight [-stdv, stdv]
        float stdv = 1. / (float)sqrt((double)em_dim * 2);
        for (i = 0; i < em_dim * category_num; i++)
        {
            w[i] = (float)rand() / RAND_MAX * 2. * stdv - stdv;
            w_bi[i] = (float)rand() / RAND_MAX * 2. * stdv - stdv;
            w_positional[i] = (float)rand() / RAND_MAX * 2. * stdv - stdv;
            w_bi_positional[i] = (float)rand() / RAND_MAX * 2. * stdv - stdv;
        }

        for (i = 0; i < category_num; i++)
            b[i] = (float)rand() / RAND_MAX * 2. * stdv - stdv;
    }
    else
    {
        for (i = 0; i < em_dim * vocab_num; i++)
        {
            em[i] = 0.;
            em_bi[i] = 0.;
        }
        for (i = 0; i < em_dim * max_text_len; i++)
        {
            em_pos[i] = 0.;
            em_bi_pos[i] = 0.;
        }
        for (i = 0; i < em_dim * category_num; i++)
        {
            w[i] = 0.;
            w_bi[i] = 0.;
            w_positional[i] = 0.;
            w_bi_positional[i] = 0.;
        }

        for (i = 0; i < category_num; i++)
            b[i] = 0.;
    }
}
void free_model(struct model_t *model)
{
    free(model->em);
    free(model->em_pos);
    free(model->em_bi);
    free(model->em_bi_pos);
    free(model->w);
    free(model->w_bi);
    free(model->w_positional);
    free(model->w_bi_positional);
    free(model->b);
}

int preread(FILE *fp)
{
    int ch = fgetc(fp);
    if (ch == EOF)
        return ch;
    else
    {
        fseek(fp, -1, SEEK_CUR);  // set the stream -fp- to a new position
        return ch;
    }
}
void load_data(struct dataset_t *data, const char *path, int64_t max_voc)  //max_voc = max word index
{
    FILE *fp = NULL;
    fp = fopen(path, "r");
    if (fp == NULL)
    {
        perror("error");
        exit(EXIT_FAILURE);
    }
    int next_i, next_ch;
    int64_t /*number of line*/text_num = 0, ch_num = 0, ignore_text_num = 0;
    int64_t text_len = 0;  // length of each sequence
    ;
    int64_t cat, text_i;
    enum state_t
    {
        READ_CAT,  // category
        READ_INDEX
    } state = READ_CAT;
    while (1)
    {
        int is_break = 0;
        switch (state)
        {
        case READ_CAT:  // read categoty label
            if (fscanf(fp, "%ld,", &cat) > 0)
            {
                if (preread(fp) == '\n')  // empty line
                {
                    ignore_text_num++;
                    fgetc(fp);  // read '\n'
                }
                else
                    state = READ_INDEX;
            }
            else // end of file
            {
                assert(feof(fp));  //expression == false, abort; feof() == non-zero, is end
                is_break = 1;
            }
            break;
        case READ_INDEX:  // read word index
            assert(fscanf(fp, "%ld", &text_i) > 0);  
            if (text_i < max_voc)  // current word in the vocabulary
            {
                ch_num++;
                text_len++;
            }
            next_ch = fgetc(fp);  // read ' ' or '\n'
            if (next_ch == '\n')  // end of current word-sequence
            {
                if (text_len == 0)  // empty line
                {
                    ignore_text_num++;
                }
                else
                {
                    text_num++;  // increase the number of word-sequence
                    text_len = 0;  // reset the length of word-sequence
                }
                state = READ_CAT;
            }
        }
        if (is_break)
            break;
    }
    printf("load data from %s\n", path);
    printf("#lines: %ld, #chs: %ld\n", text_num, ch_num);
    printf("#ignore lines: %ld\n", ignore_text_num);
    data->text_num = text_num;
    data->text_indices = (int64_t *)malloc(ch_num * sizeof(int64_t));
    data->text_lens = (int64_t *)malloc(text_num * sizeof(int64_t));  // length of each word-sequence
    data->text_categories = (int64_t *)malloc(text_num * sizeof(int64_t));
    data->start_pos = (int64_t *)malloc(text_num * sizeof(int64_t));

    text_len = 0;
    int64_t *text_indices = data->text_indices;
    int64_t *text_lens = data->text_lens;
    int64_t *text_categories = data->text_categories;
    int64_t *start_pos = data->start_pos;
    rewind(fp);  // set position of stream to the beginning
    while (1)
    {
        int is_break = 0;
        switch (state)
        {
        case READ_CAT:  // read category label
            if (fscanf(fp, "%ld,", &cat) > 0)
            {
                if (preread(fp) == '\n')  // empty line
                {
                    fgetc(fp);  // read '\n'
                }
                else
                    state = READ_INDEX;
            }
            else  // end of file
            {
                assert(feof(fp));  //expression == false -> abort; feof() == non-zero -> is end
                is_break = 1;
            }
            break;
        case READ_INDEX:  // read word index
            assert(fscanf(fp, "%ld", &text_i) > 0);
            if (text_i < max_voc)  // current word in the vocabulary
            {
                text_len++;
                *text_indices = text_i;
                text_indices++;
            }
            next_ch = fgetc(fp);  // read ' ' or '\n'
            if (next_ch == '\n')  // end of current word-sequence
            {
                state = READ_CAT;
                if (text_len > 0)
                {
                    *text_lens = text_len;
                    text_lens++;
                    text_len = 0;  // reset the length of word-sequence

                    *text_categories = cat;  // set category label
                    text_categories++;
                }
            }
        }
        if (is_break)
            break;
    }
    start_pos[0] = 0;  // number of line
    for (int64_t i = 1; i < text_num; i++)
        start_pos[i] = start_pos[i - 1] + data->text_lens[i - 1];  // current pos = previous pos + previous length
    fclose(fp);
}

void free_data(struct dataset_t *data)
{
    free(data->text_indices);
    free(data->text_lens);
    free(data->text_categories);
    free(data->start_pos);
}

float forward(struct model_t *model, struct dataset_t *train_data, int64_t text_i, float *ave_fea, int64_t *ave_fea_index,\
 float *max_bi_fea, int64_t *max_bi_fea_index,\
 float *max_positional_fea, int64_t *max_positional_fea_index, int64_t *max_positional_em_index,\
 float *max_bi_positional_fea, int64_t *max_bi_positional_fea_index, int64_t *max_bi_positional_em_index, float *softmax_fea)
{  // load text_i th word-sequence
    int64_t *text_indices = &(train_data->text_indices[train_data->start_pos[text_i]]);
    int64_t text_len = train_data->text_lens[text_i];
    assert(text_len >= 1);  // expression == true -> pass
    int64_t text_category = train_data->text_categories[text_i];

    int64_t i, j;
    int64_t em_index, em_index0, em_index1;

    // max_pool original + average
    *ave_fea_index = text_i;
    for (i = 0; i < model->em_dim; i++)
    {
        ave_fea[i] = 0.;

    }

    for (i = 0; i < text_len; i++)
    {
        em_index = text_indices[i] * model->em_dim;
        for (j = 0; j < model->em_dim; j++)
        {
            ave_fea[j] += model->em[em_index + j];
        }
    }

    for (i = 0; i < model->em_dim; i++)
    {
        ave_fea[i] /= text_len;
    }

    // max_pool bi
    // 先赋预值
    em_index0 = text_indices[0] * model->em_dim;
    em_index1 = (text_len > 1) ? (text_indices[1] * model->em_dim) : (text_indices[0] * model->em_dim); //长度为1 那么就把那个单词复制一个
    for (j = 0; j < model->em_dim; j++)
    {
        max_bi_fea[j] = (model->em_bi[em_index0 + j] + model->em_bi[em_index1 + j])*0.5;  // take average
        max_bi_fea_index[0] = em_index0 + j;
        max_bi_fea_index[1] = em_index1 + j;
    }
    
    if (text_len == 1)
    {
        // printf("warning: text[id: %ld] length == 1 (bi-gram features need length>1)\n", text_i);
    }

    for (i = 1; i < text_len - 1; i++)
    {
        em_index0 = text_indices[i] * model->em_dim;
        em_index1 = text_indices[i + 1] * model->em_dim;

        for (j = 0; j < model->em_dim; j++)
        {
            float fea = (model->em_bi[em_index0 + j] + model->em_bi[em_index1 + j])*0.5;  // take average
            if (max_bi_fea[j] < fea)
            {
                max_bi_fea[j] = fea;
                max_bi_fea_index[2 * j] = em_index0 + j;
                max_bi_fea_index[2 * j + 1] = em_index1 + j;
            }
        }
    }

    // max_pooling positional embedding
    // 先赋预值
    em_index = text_indices[0] * model->em_dim;
    for (j = 0; j < model->em_dim; j++)
    {
        max_positional_fea[j] = model->em[em_index + j] + LAMBDA * model->em_pos[j];
        max_positional_fea_index[j] = em_index + j;
        max_positional_em_index[j] = j;  // 0 * em_dim + j
    }

    for (i = 1; i < text_len; i++)
    {
        em_index = text_indices[i] * model->em_dim;
        for (j = 0; j < model->em_dim; j++)
        {
            float pos_fea = model->em[em_index + j] + LAMBDA * model->em_pos[i * model->em_dim + j];
            if (max_positional_fea[j] < pos_fea)
            {
                max_positional_fea[j] =  pos_fea;
                max_positional_fea_index[j] = em_index + j;
                max_positional_em_index[j] = i * model->em_dim + j;
            }
        }
    }

    // max pooling bi positional embedding
    em_index0 = text_indices[0] * model->em_dim;
    em_index1 = (text_len > 1) ? (text_indices[1] * model->em_dim) : (text_indices[0] * model->em_dim); //长度为1 那么就把那个单词复制一个
    for (j = 0; j < model->em_dim; j++)
    {
        max_bi_positional_fea[j] = (model->em_bi[em_index0 + j] + model->em_bi[em_index1 + j]\
         + LAMBDA * model->em_bi_pos[j] + LAMBDA * model->em_bi_pos[model->em_dim + j])*0.5;  // take average
        max_bi_positional_fea_index[0] = em_index0 + j;
        max_bi_positional_fea_index[1] = em_index1 + j;
        max_bi_positional_em_index[0] = j;
        max_bi_positional_em_index[1] = model->em_dim + j;
    }
    
    if (text_len == 1)
    {
        // printf("warning: text[id: %ld] length == 1 (bi-gram features need length>1)\n", text_i);
    }

    for (i = 1; i < text_len - 1; i++)
    {
        em_index0 = text_indices[i] * model->em_dim;
        em_index1 = text_indices[i + 1] * model->em_dim;

        for (j = 0; j < model->em_dim; j++)
        {
            float fea = (model->em_bi[em_index0 + j] + model->em_bi[em_index1 + j]\
             + LAMBDA * model->em_bi_pos[i * model->em_dim + j] + LAMBDA * model->em_bi_pos[(i + 1) * model->em_dim + j])*0.5;  // take average
            if (max_bi_positional_fea[j] < fea)
            {
                max_bi_positional_fea[j] = fea;
                max_bi_positional_fea_index[2 * j] = em_index0 + j;
                max_bi_positional_fea_index[2 * j + 1] = em_index1 + j;
                max_bi_positional_em_index[2 * j] = i * model->em_dim + j;
                max_bi_positional_em_index[2 * j + 1] = (i + 1) * model->em_dim + j;
            }
        }
    }

    // mlp
    for (i = 0; i < model->category_num; i++)
        softmax_fea[i] = model->b[i];

    for (i = 0; i < model->category_num; i++)
        for (j = 0; j < model->em_dim; j++)
            softmax_fea[i] += (ave_fea[j] * model->w[i * model->em_dim + j]\
             + max_bi_fea[j] * model->w_bi[i * model->em_dim + j]\
             + max_positional_fea[j] * model->w_positional[i * model->em_dim + j]\
             + max_bi_positional_fea[j] * model->w_bi_positional[i * model->em_dim + j]);
    
    float loss = 0.;
    float tmp = 0.;
    loss -= softmax_fea[text_category];  // = log(exp(softmax))
    for (i = 0; i < model->category_num; i++)
    {
        softmax_fea[i] = (float)exp((double)softmax_fea[i]);
        tmp += softmax_fea[i];
    }
    loss += (float)log(tmp);  // loss = -log(exp(softmax/sigma(exp))
                               //      = log(sigma(exp)/exp(softmax)
                               //      = loh(sigma(exp)) - softmax
    return loss;
}

void backward(struct model_t *model, struct dataset_t *train_data, int64_t text_i, float *ave_fea, float *max_bi_fea,\
 float *max_positional_fea, float *max_bi_positional_fea, float *softmax_fea,\
 float *grad_em_ave, float *grad_em_pos, float *grad_em_bi, float *grad_em_bi_pos, float *grad_w, float *grad_w_bi, float *grad_w_positional, float *grad_w_bi_positional, float *grad_b)
{  // load text_i th word-sequence
    int64_t *text_indices = &(train_data->text_indices[train_data->start_pos[text_i]]);
    int64_t text_len = train_data->text_lens[text_i];
    int64_t text_category = train_data->text_categories[text_i];

    float tmp_sum = 0.;
    int64_t i, j;
    for (i = 0; i < model->category_num; i++)
        tmp_sum += softmax_fea[i];
    for (i = 0; i < model->category_num; i++)
        grad_b[i] = softmax_fea[i] / tmp_sum;
    grad_b[text_category] -= 1.;  // error[text_category] = pro_softmax - 1
                                  // error[other] = pro_softmax - 0
                                  // error = predicted - labelled

    // original weight
    for (i = 0; i < model->category_num; i++)
        for (j = 0; j < model->em_dim; j++)
            grad_w[i * model->em_dim + j] = ave_fea[j] * grad_b[i];

    // bi weight
    for (i = 0; i < model->category_num; i++)
        for (j = 0; j < model->em_dim; j++)
            grad_w_bi[i * model->em_dim + j] = max_bi_fea[j] * grad_b[i];

    // positional weight
    for (i = 0; i < model->category_num; i++)
        for (j = 0; j < model->em_dim; j++)
            grad_w_positional[i * model->em_dim + j] = max_positional_fea[j] * grad_b[i];

    // bi positional weight
    for (i = 0; i < model->category_num; i++)
        for (j = 0; j < model->em_dim; j++)
            grad_w_bi_positional[i * model->em_dim + j] = max_bi_positional_fea[j] * grad_b[i];

    // original look up table - average
    for (j = 0; j < model->em_dim; j++)
        grad_em_ave[j] = 0.;
    for (i = 0; i < model->category_num; i++)
        for (j = 0; j < model->em_dim; j++)
            grad_em_ave[j] += (model->w[i * model->em_dim + j]) * grad_b[i];

    // original look up table - positinoal embedding
    for (j = 0; j < model->em_dim; j++)
        grad_em_pos[j] = 0.;
    for (i = 0; i < model->category_num; i++)
        for (j = 0; j < model->em_dim; j++)
            grad_em_pos[j] += (model->w_positional[i * model->em_dim + j]) * grad_b[i];

    // bi look up table
    for (j = 0; j < model->em_dim; j++)
        grad_em_bi[j] = 0.;
    for (i = 0; i < model->category_num; i++)
        for (j = 0; j < model->em_dim; j++)
            grad_em_bi[j] += (model->w_bi[i * model->em_dim + j]) * grad_b[i];

    // bi look up table - positinal embedding
    for (j = 0; j < model->em_dim; j++)
        grad_em_bi_pos[j] = 0.;
    for (i = 0; i < model->category_num; i++)
        for (j = 0; j < model->em_dim; j++)
            grad_em_bi_pos[j] += (model->w_bi_positional[i * model->em_dim + j]) * grad_b[i];
}

void evaluate(struct model_t *model, struct dataset_t *vali_data, int64_t batch_size, int64_t threads_n)
{
    printf("evaluating...\n");

    time_t eva_start, eva_end;
    eva_start = time(NULL);

    float *ave_feas = (float *)malloc(model->em_dim * batch_size * sizeof(float));  // max feature of original + average
    int64_t *ave_fea_indexs = (int64_t *)malloc(batch_size * sizeof(int64_t));
    float *max_bi_feas = (float *)malloc(model->em_dim * batch_size * sizeof(float));  // max feature of bi-gram
    int64_t *max_bi_fea_indexs = (int64_t *)malloc(2 * model->em_dim * batch_size * sizeof(int64_t));
    float *max_positional_feas = (float *)malloc(model->em_dim * batch_size * sizeof(float));  // max feature of original + positional
    int64_t *max_positional_fea_indexs = (int64_t *)malloc(model->em_dim * batch_size * sizeof(int64_t));
    int64_t *max_positional_em_indexs = (int64_t *)malloc(model->em_dim * batch_size * sizeof(int64_t));
    float *max_bi_positional_feas = (float *)malloc(model->em_dim * batch_size * sizeof(float));  // max feature of bi-gram + positional
    int64_t *max_bi_positional_fea_indexs = (int64_t *)malloc(2 * model->em_dim * batch_size * sizeof(int64_t));
    int64_t *max_bi_positional_em_indexs = (int64_t *)malloc(2 * model->em_dim * batch_size * sizeof(int64_t));
    float *softmax_feas = (float *)malloc(model->category_num * batch_size * sizeof(float)); // input of softmax?

    int64_t *pre_labels = (int64_t *)malloc(batch_size * sizeof(int64_t));  // predicted
    int64_t *real_labels = (int64_t *)malloc(batch_size * sizeof(int64_t));
    // 临界资源
    float *cat_all = (float *)malloc(model->category_num * sizeof(float));
    float *cat_true = (float *)malloc(model->category_num * sizeof(float));

    for (int64_t i = 0; i < model->category_num; i++)
    {
        cat_all[i] = 0.;
        cat_true[i] = 0.;
    }

    for (int64_t batch_i = 0; batch_i < (vali_data->text_num + batch_size - 1) / batch_size; batch_i++)
    {// batch_i: index of batch
        int64_t real_batch_size = (vali_data->text_num - batch_i * batch_size) > batch_size ? batch_size : (vali_data->text_num - batch_i * batch_size);
        // 可以加速
#pragma omp parallel for schedule(dynamic) num_threads(threads_n)
        for (int64_t batch_j = 0; batch_j < real_batch_size; batch_j++)
        {// batch_j: index inside each batch
            int64_t text_i = (batch_i)*batch_size + batch_j;  // index of line
            assert(text_i < vali_data->text_num);  // expression == true -> pass

            int64_t text_category = vali_data->text_categories[text_i];
            // 长度为0的text，不计算梯度
            // 会导致问题，比如梯度没有更新
            // 应该在生成数据时避免
            if (vali_data->text_lens[text_i] == 0)
            {
                printf("error: vali text length can not be zero.[text id: %ld]", text_i);
                exit(-1);
            }

            float *ave_fea = &ave_feas[batch_j * model->em_dim];
            int64_t *ave_fea_index = &ave_fea_indexs[batch_j];
            float *max_bi_fea = &max_bi_feas[batch_j * model->em_dim];
            int64_t *max_bi_fea_index = &max_bi_fea_indexs[2 * batch_j * model->em_dim];
            float *max_positional_fea = &max_positional_feas[batch_j * model->em_dim];
            int64_t *max_positional_fea_index = &max_positional_fea_indexs[batch_j * model->em_dim];
            int64_t *max_positional_em_index = &max_positional_em_indexs[batch_j * model->em_dim];
            float *max_bi_positional_fea = &max_bi_positional_feas[batch_j * model->em_dim];
            int64_t *max_bi_positional_fea_index = &max_bi_positional_fea_indexs[2 * batch_j * model->em_dim];
            int64_t *max_bi_positional_em_index = &max_bi_positional_em_indexs[2 * batch_j * model->em_dim];
            float *softmax_fea = &softmax_feas[batch_j * model->category_num];

            int64_t *pre_label = &pre_labels[batch_j];  // predicted
            int64_t *real_label = &real_labels[batch_j];

            *real_label = text_category;

            forward(model, vali_data, text_i, ave_fea, ave_fea_index, max_bi_fea, max_bi_fea_index,\
             max_positional_fea, max_positional_fea_index, max_positional_em_index,\
             max_bi_positional_fea, max_bi_positional_fea_index, max_bi_positional_em_index, softmax_fea);
            *pre_label = 0;
            float fea = softmax_fea[0];
            for (int64_t c = 1; c < model->category_num; c++)
            { // find max posibility (predicted category)
                if (softmax_fea[c] > fea)
                {
                    *pre_label = c;
                    fea = softmax_fea[c];
                }
            }
        }

        // 访问临界资源
        for (int64_t batch_j = 0; batch_j < real_batch_size; batch_j++)
        {// do statistics
            cat_all[real_labels[batch_j]] += 1;
            if (real_labels[batch_j] == pre_labels[batch_j])
                cat_true[real_labels[batch_j]] += 1;
        }
    }
    float cat_all_sum = 0.;
    float cat_true_sum = 0.;
    for (int64_t k = 0; k < model->category_num; k++)
    {
        cat_all_sum += cat_all[k];
        cat_true_sum += cat_true[k];
    }

    printf("#samples: %.0f\n", cat_all_sum);
    FILE *fp = fopen("fntext_bi_mod_lea_new_la0.1_10_500.txt", "a");
    printf("macro precision: %.5f\n", cat_true_sum / cat_all_sum);
    fprintf(fp, "%.5f\n", 100 * cat_true_sum / cat_all_sum);
    for (int64_t k = 0; k < model->category_num; k++)
    {
        printf("   category #%ld precision: %.5f\n", k, cat_true[k] / cat_all[k]);
        //fprintf(fp, "   category #%ld precision: %.5f\n", k, cat_true[k] / cat_all[k]);
    }
    fclose(fp);

    free(ave_feas);
    free(ave_fea_indexs);
    free(max_bi_feas);
    free(max_bi_fea_indexs);
    free(max_positional_feas);
    free(max_positional_fea_indexs);
    free(max_positional_em_indexs);
    free(max_bi_positional_feas);
    free(max_bi_positional_fea_indexs);
    free(max_bi_positional_em_indexs);
    free(softmax_feas);

    free(pre_labels);
    free(real_labels);
    free(cat_all);
    free(cat_true);

    eva_end = time(NULL);
    printf("   evaluating time: %lds\n", eva_end - eva_start);
}

void train_adam(struct model_t *model, struct dataset_t *train_data, struct dataset_t *vali_data, int64_t epochs, int64_t batch_size, int64_t threads_n)
{
    printf("start training(Adam)...\n");
    //     omp_lock_t omplock;
    // omp_init_lock(&omplock);

    int64_t tmp, i, sel, max_text_len = 0;

    float alpha = 0.001, beta1 = 0.9, beta2 = 0.999, epsilon = 1e-8;
    float beta1t = beta1;
    float beta2t = beta2;

    int64_t *shuffle_index = (int64_t *)malloc(train_data->text_num * sizeof(int64_t));  // number of line

    for (i = 0; i < train_data->text_num; i++)
        if (max_text_len < train_data->text_lens[i])
            max_text_len = train_data->text_lens[i];

    struct model_t adam_m, adam_v, gt;
    init_model(&adam_m, model->em_dim, model->vocab_num, model->category_num, max_text_len, 0);
    init_model(&adam_v, model->em_dim, model->vocab_num, model->category_num, max_text_len, 0);
    init_model(&gt, model->em_dim, model->vocab_num, model->category_num, max_text_len, 0);

    float *grads_em_ave = (float *)malloc(model->em_dim * batch_size * sizeof(float));
    float *grads_em_pos = (float *)malloc(model->em_dim * batch_size * sizeof(float));
    float *grads_em_bi = (float *)malloc(model->em_dim * batch_size * sizeof(float));
    float *grads_em_bi_pos = (float *)malloc(model->em_dim * batch_size * sizeof(float));
    float *grads_w = (float *)malloc(model->em_dim * model->category_num * batch_size * sizeof(float));
    float *grads_w_bi = (float *)malloc(model->em_dim * model->category_num * batch_size * sizeof(float));
    float *grads_w_positional = (float *)malloc(model->em_dim * model->category_num * batch_size * sizeof(float));
    float *grads_w_bi_positional = (float *)malloc(model->em_dim * model->category_num * batch_size * sizeof(float));
    float *grads_b = (float *)malloc(model->category_num * batch_size * sizeof(float));

    float *ave_feas = (float *)malloc(model->em_dim * batch_size * sizeof(float));
    int64_t *ave_fea_indexs = (int64_t *)malloc(batch_size * sizeof(int64_t));
    float *max_bi_feas = (float *)malloc(model->em_dim * batch_size * sizeof(float));
    int64_t *max_bi_fea_indexs = (int64_t *)malloc(2 * model->em_dim * batch_size * sizeof(int64_t));
    float *max_positional_feas = (float *)malloc(model->em_dim * batch_size * sizeof(float));
    int64_t *max_positional_fea_indexs = (int64_t *)malloc(model->em_dim * batch_size * sizeof(int64_t));
    int64_t *max_positional_em_indexs = (int64_t *)malloc(model->em_dim * batch_size * sizeof(int64_t));
    float *max_bi_positional_feas = (float *)malloc(model->em_dim * batch_size * sizeof(float));
    int64_t *max_bi_positional_fea_indexs = (int64_t *)malloc(2 * model->em_dim * batch_size * sizeof(int64_t));
    int64_t *max_bi_positional_em_indexs = (int64_t *)malloc(2 * model->em_dim * batch_size * sizeof(int64_t));
    float *softmax_feas = (float *)malloc(model->category_num * batch_size * sizeof(float));
    float *losses = (float *)malloc(batch_size * sizeof(float));

    printf("init grad end...\n");

    for (i = 0; i < train_data->text_num; i++)
        shuffle_index[i] = i;

    for (int64_t epoch = 0; epoch < epochs; epoch++)
    {
        printf("#epoch: %ld\n", epoch);
        float s_loss = 0.;
        time_t epoch_start, epoch_end;
        // clock_t epoch_start, epoch_end;
        // shuffle
        for (i = 0; i < train_data->text_num; i++)
        {
            sel = rand() % (train_data->text_num - i) + i;  // rand int in [i, text_num)
            tmp = shuffle_index[i];
            shuffle_index[i] = shuffle_index[sel];
            shuffle_index[sel] = tmp;
        }

        epoch_start = time(NULL);
        // epoch_start = clock();
        for (int64_t batch_i = 0; batch_i < (train_data->text_num + batch_size - 1) / batch_size; batch_i++)
        {
            int64_t real_batch_size = (train_data->text_num - batch_i * batch_size) > batch_size ? batch_size : (train_data->text_num - batch_i * batch_size);
            // 可以加速
#pragma omp parallel for schedule(dynamic) num_threads(threads_n)
            for (int64_t batch_j = 0; batch_j < real_batch_size; batch_j++)
            {
                int64_t text_i = (batch_i)*batch_size + batch_j;
                assert(text_i < train_data->text_num);  // expression = true -> pass
                text_i = shuffle_index[text_i];

                // 长度为0的text，不计算梯度
                // 会导致问题，比如梯度没有更新
                // 应该在生成数据时避免
                if (train_data->text_lens[text_i] == 0)
                {
                    printf("error: training text length can not be zero.[text id: %ld]", text_i);
                    exit(-1);
                }

                float *grad_em_ave = &grads_em_ave[batch_j * model->em_dim];
                float *grad_em_pos = &grads_em_pos[batch_j * model->em_dim];
                float *grad_em_bi = &grads_em_bi[batch_j * model->em_dim];
                float *grad_em_bi_pos = &grads_em_bi_pos[batch_j * model->em_dim];
                float *grad_w = &grads_w[batch_j * model->em_dim * model->category_num];
                float *grad_w_bi = &grads_w_bi[batch_j * model->em_dim * model->category_num];
                float *grad_w_positional = &grads_w_positional[batch_j * model->em_dim * model->category_num];
                float *grad_w_bi_positional = &grads_w_bi_positional[batch_j * model->em_dim * model->category_num];
                float *grad_b = &grads_b[batch_j * model->category_num];

                float *ave_fea = &ave_feas[batch_j * model->em_dim];
                int64_t *ave_fea_index = &ave_fea_indexs[batch_j];
                float *max_bi_fea = &max_bi_feas[batch_j * model->em_dim];
                int64_t *max_bi_fea_index = &max_bi_fea_indexs[2 * batch_j * model->em_dim];
                float *max_positional_fea = &max_positional_feas[batch_j * model->em_dim];
                int64_t *max_positional_fea_index = &max_positional_fea_indexs[batch_j * model->em_dim];
                int64_t *max_positional_em_index = &max_positional_em_indexs[batch_j * model->em_dim];
                float *max_bi_positional_fea = &max_bi_positional_feas[batch_j * model->em_dim];
                int64_t *max_bi_positional_fea_index = &max_bi_positional_fea_indexs[2 * batch_j * model->em_dim];
                int64_t *max_bi_positional_em_index = &max_bi_positional_em_indexs[2 * batch_j * model->em_dim];
                float *softmax_fea = &softmax_feas[batch_j * model->category_num];

                losses[batch_j] = forward(model, train_data, text_i, ave_fea, ave_fea_index, max_bi_fea, max_bi_fea_index,\
                 max_positional_fea, max_positional_fea_index, max_positional_em_index,\
                 max_bi_positional_fea, max_bi_positional_fea_index, max_bi_positional_em_index, softmax_fea);
                backward(model, train_data, text_i, ave_fea, max_bi_fea, max_positional_fea, max_bi_positional_fea, softmax_fea,\
                 grad_em_ave, grad_em_pos, grad_em_bi, grad_em_bi_pos, grad_w, grad_w_bi, grad_w_positional, grad_w_bi_positional, grad_b);
            }

            for (int64_t batch_j = 0; batch_j < real_batch_size; batch_j++)
                s_loss += losses[batch_j];
            // 把多个batch的梯度累加起来 不可以加速，因为gt.em是临界资源
            for (int64_t batch_j = 0; batch_j < real_batch_size; batch_j++)
            {
                for (int64_t batch_k = 0; batch_k < model->em_dim * model->category_num; batch_k++)
                {
                    gt.w[batch_k] += grads_w[batch_j * model->em_dim * model->category_num + batch_k] / (float)batch_size;  // original
                    gt.w_bi[batch_k] += grads_w_bi[batch_j * model->em_dim * model->category_num + batch_k] / (float)batch_size;  // bi   
                    gt.w_positional[batch_k] += grads_w_positional[batch_j * model->em_dim * model->category_num + batch_k] / (float)batch_size;  // original positional
                    gt.w_bi_positional[batch_k] += grads_w_bi_positional[batch_j * model->em_dim * model->category_num + batch_k] / (float)batch_size;  // bi positional
                }
                for (int64_t batch_k = 0; batch_k < model->category_num; batch_k++)
                {
                    gt.b[batch_k] += grads_b[batch_j * model->category_num + batch_k] / (float)batch_size;
                }
                // em的grad 特殊对待
                for (int64_t batch_k = 0; batch_k < model->em_dim; batch_k++)
                {
                    // back propagation of max pooling to original look up table
                    int64_t em_index = max_positional_fea_indexs[batch_j * model->em_dim + batch_k];
                    gt.em[em_index] += grads_em_pos[batch_j * model->em_dim + batch_k] / (float)batch_size;

                    // back propagation of average pooling
                    int64_t em_text_index = ave_fea_indexs[batch_j];
                    for (int64_t text_j = 0; text_j < train_data->text_lens[em_text_index]; text_j++)
                    {
                        int64_t start_pos_index = train_data->start_pos[em_text_index];
                        em_index = train_data->text_indices[start_pos_index + text_j] * model->em_dim + batch_k;
                        gt.em[em_index] += grads_em_ave[batch_j * model->em_dim + batch_k] / ((float)batch_size * train_data->text_lens[em_text_index]);
                    }

                    // back prpagation of max pooling to positional embedding look up table
                    em_index = max_positional_em_indexs[batch_j * model->em_dim + batch_k];
                    gt.em_pos[em_index] += LAMBDA * grads_em_pos[batch_j * model->em_dim + batch_k] / (float)batch_size;

                    // bi + positional embedding
                    int64_t em_index0 = max_bi_positional_fea_indexs[2 * batch_j * model->em_dim + 2 * batch_k];
                    int64_t em_index1 = max_bi_positional_fea_indexs[2 * batch_j * model->em_dim + 2 * batch_k + 1];
                    gt.em_bi[em_index0] += 0.5 * grads_em_bi_pos[batch_j * model->em_dim + batch_k] / (float)batch_size;  // take average
                    gt.em_bi[em_index1] += 0.5 * grads_em_bi_pos[batch_j * model->em_dim + batch_k] / (float)batch_size;  // take average

                    // bi
                    em_index0 = max_bi_fea_indexs[2 * batch_j * model->em_dim + 2 * batch_k];
                    em_index1 = max_bi_fea_indexs[2 * batch_j * model->em_dim + 2 * batch_k + 1];
                    gt.em_bi[em_index0] += 0.5 * grads_em_bi[batch_j * model->em_dim + batch_k] / (float)batch_size;  // take average
                    gt.em_bi[em_index1] += 0.5 * grads_em_bi[batch_j * model->em_dim + batch_k] / (float)batch_size;  // take average

                    // bi positional embedding look up table
                    em_index0 = max_bi_positional_em_indexs[2 * batch_j * model->em_dim + 2 * batch_k];
                    em_index1 = max_bi_positional_em_indexs[2 * batch_j * model->em_dim + 2 * batch_k + 1];
                    gt.em_bi_pos[em_index0] += 0.5 * LAMBDA * grads_em_bi_pos[batch_j * model->em_dim + batch_k] / (float)batch_size;  // take average
                    gt.em_bi_pos[em_index1] += 0.5 * LAMBDA * grads_em_bi_pos[batch_j * model->em_dim + batch_k] / (float)batch_size;  // take average
                }
            }

                // 计算m,v update param 可以加速
#pragma omp parallel for schedule(static) num_threads(threads_n)
            for (int64_t batch_k = 0; batch_k < model->em_dim * model->category_num; batch_k++)
            {
                // original
                adam_m.w[batch_k] = beta1 * adam_m.w[batch_k] + (1 - beta1) * gt.w[batch_k];
                adam_v.w[batch_k] = beta2 * adam_v.w[batch_k] + (1 - beta2) * gt.w[batch_k] * gt.w[batch_k];
                gt.w[batch_k] = 0.;

                float m_hat = adam_m.w[batch_k] / (1 - beta1t);
                float v_hat = adam_v.w[batch_k] / (1 - beta2t);
                model->w[batch_k] -= alpha * m_hat / ((float)sqrt((float)v_hat) + epsilon);

                // bi
                adam_m.w_bi[batch_k] = beta1 * adam_m.w_bi[batch_k] + (1 - beta1) * gt.w_bi[batch_k];
                adam_v.w_bi[batch_k] = beta2 * adam_v.w_bi[batch_k] + (1 - beta2) * gt.w_bi[batch_k] * gt.w_bi[batch_k];
                gt.w_bi[batch_k] = 0.;

                m_hat = adam_m.w_bi[batch_k] / (1 - beta1t);
                v_hat = adam_v.w_bi[batch_k] / (1 - beta2t);
                model->w_bi[batch_k] -= alpha * m_hat / ((float)sqrt((float)v_hat) + epsilon);

                // original positional
                adam_m.w_positional[batch_k] = beta1 * adam_m.w_positional[batch_k] + (1 - beta1) * gt.w_positional[batch_k];
                adam_v.w_positional[batch_k] = beta2 * adam_v.w_positional[batch_k] + (1 - beta2) * gt.w_positional[batch_k] * gt.w_positional[batch_k];
                gt.w_positional[batch_k] = 0.;

                m_hat = adam_m.w_positional[batch_k] / (1 - beta1t);
                v_hat = adam_v.w_positional[batch_k] / (1 - beta2t);
                model->w_positional[batch_k] -= alpha * m_hat / ((float)sqrt((float)v_hat) + epsilon);

                // bi positional
                adam_m.w_bi_positional[batch_k] = beta1 * adam_m.w_bi_positional[batch_k] + (1 - beta1) * gt.w_bi_positional[batch_k];
                adam_v.w_bi_positional[batch_k] = beta2 * adam_v.w_bi_positional[batch_k] + (1 - beta2) * gt.w_bi_positional[batch_k] * gt.w_bi_positional[batch_k];
                gt.w_bi_positional[batch_k] = 0.;

                m_hat = adam_m.w_bi_positional[batch_k] / (1 - beta1t);
                v_hat = adam_v.w_bi_positional[batch_k] / (1 - beta2t);
                model->w_bi_positional[batch_k] -= alpha * m_hat / ((float)sqrt((float)v_hat) + epsilon);
            }

            // 循环数量少，不用加速
            for (int64_t batch_k = 0; batch_k < model->category_num; batch_k++)
            {
                adam_m.b[batch_k] = beta1 * adam_m.b[batch_k] + (1 - beta1) * gt.b[batch_k];
                adam_v.b[batch_k] = beta2 * adam_v.b[batch_k] + (1 - beta2) * gt.b[batch_k] * gt.b[batch_k];
                gt.b[batch_k] = 0.;

                float m_hat = adam_m.b[batch_k] / (1 - beta1t);
                float v_hat = adam_v.b[batch_k] / (1 - beta2t);
                model->b[batch_k] -= alpha * m_hat / ((float)sqrt((float)v_hat) + epsilon);
            }

            // adam_m,adam_v,model->em, gt.em是临界资源
            for (int64_t batch_j = 0; batch_j < real_batch_size; batch_j++)
            {
                // em的grad 特殊对待
                for (int64_t batch_k = 0; batch_k < model->em_dim; batch_k++)
                {
                    // average pooling
                    int64_t em_text_index = ave_fea_indexs[batch_j];
                    for (int64_t text_j = 0; text_j < train_data->text_lens[em_text_index]; text_j++)
                    {
                        int64_t start_pos_index = train_data->start_pos[em_text_index];
                        int64_t em_index = train_data->text_indices[start_pos_index + text_j] * model->em_dim + batch_k;  // average pooling index include max pooling
                        if (gt.em[em_index] != 0.)
                        {
                            adam_m.em[em_index] = beta1 * adam_m.em[em_index] + (1 - beta1) * gt.em[em_index];
                            adam_v.em[em_index] = beta2 * adam_v.em[em_index] + (1 - beta2) * gt.em[em_index] * gt.em[em_index];
                            gt.em[em_index] = 0.;

                            float m_hat = adam_m.em[em_index] / (1 - beta1t);
                            float v_hat = adam_v.em[em_index] / (1 - beta2t);
                            model->em[em_index] -= alpha * m_hat / ((float)sqrt((float)v_hat) + epsilon);
                        }
                    }

                    // max pooling postional embedding look up table
                    int64_t em_index = max_positional_em_indexs[batch_j * model->em_dim + batch_k];
                    if (gt.em_pos[em_index] != 0.)
                    {
                        adam_m.em_pos[em_index] = beta1 * adam_m.em_pos[em_index] + (1 - beta1) * gt.em_pos[em_index];
                        adam_v.em_pos[em_index] = beta2 * adam_v.em_pos[em_index] + (1 - beta2) * gt.em_pos[em_index] * gt.em_pos[em_index];
                        gt.em_pos[em_index] = 0.;

                        float m_hat = adam_m.em_pos[em_index] / (1 - beta1t);
                        float v_hat = adam_v.em_pos[em_index] / (1 - beta2t);
                        model->em_pos[em_index] -= alpha * m_hat / ((float)sqrt((float)v_hat) + epsilon);
                    }

                    // bi
                    int64_t em_index0 = max_bi_fea_indexs[2 * batch_j * model->em_dim + 2 * batch_k];
                    int64_t em_index1 = max_bi_fea_indexs[2 * batch_j * model->em_dim + 2 * batch_k + 1];

                    if (gt.em_bi[em_index0] != 0.)
                    {
                        adam_m.em_bi[em_index0] = beta1 * adam_m.em_bi[em_index0] + (1 - beta1) * gt.em_bi[em_index0];
                        adam_v.em_bi[em_index0] = beta2 * adam_v.em_bi[em_index0] + (1 - beta2) * gt.em_bi[em_index0] * gt.em_bi[em_index0];
                        gt.em_bi[em_index0] = 0.;

                        float m_hat = adam_m.em_bi[em_index0] / (1 - beta1t);
                        float v_hat = adam_v.em_bi[em_index0] / (1 - beta2t);
                        model->em_bi[em_index0] -= alpha * m_hat / ((float)sqrt((float)v_hat) + epsilon);
                    }
                    if (gt.em_bi[em_index1] != 0.)
                    {
                        adam_m.em_bi[em_index1] = beta1 * adam_m.em_bi[em_index1] + (1 - beta1) * gt.em_bi[em_index1];
                        adam_v.em_bi[em_index1] = beta2 * adam_v.em_bi[em_index1] + (1 - beta2) * gt.em_bi[em_index1] * gt.em_bi[em_index1];
                        gt.em_bi[em_index1] = 0.;

                        float m_hat = adam_m.em_bi[em_index1] / (1 - beta1t);
                        float v_hat = adam_v.em_bi[em_index1] / (1 - beta2t);
                        model->em_bi[em_index1] -= alpha * m_hat / ((float)sqrt((float)v_hat) + epsilon);
                    }

                    // bi positional embedding
                    em_index0 = max_bi_positional_fea_indexs[2 * batch_j * model->em_dim + 2 * batch_k];
                    em_index1 = max_bi_positional_fea_indexs[2 * batch_j * model->em_dim + 2 * batch_k + 1];

                    char find_index_overlap = (em_index0 == max_bi_fea_indexs[2 * batch_j * model->em_dim + 2 * batch_k])?1:0;
                    if (gt.em_bi[em_index0] != 0. && find_index_overlap == 0)
                    {
                        adam_m.em_bi[em_index0] = beta1 * adam_m.em_bi[em_index0] + (1 - beta1) * gt.em_bi[em_index0];
                        adam_v.em_bi[em_index0] = beta2 * adam_v.em_bi[em_index0] + (1 - beta2) * gt.em_bi[em_index0] * gt.em_bi[em_index0];
                        gt.em_bi[em_index0] = 0.;

                        float m_hat = adam_m.em_bi[em_index0] / (1 - beta1t);
                        float v_hat = adam_v.em_bi[em_index0] / (1 - beta2t);
                        model->em_bi[em_index0] -= alpha * m_hat / ((float)sqrt((float)v_hat) + epsilon);
                    }
                    find_index_overlap = (em_index1 == max_bi_fea_indexs[2 * batch_j * model->em_dim + 2 * batch_k + 1])?1:0;
                    if (gt.em_bi[em_index1] != 0. && find_index_overlap == 0)
                    {
                        adam_m.em_bi[em_index1] = beta1 * adam_m.em_bi[em_index1] + (1 - beta1) * gt.em_bi[em_index1];
                        adam_v.em_bi[em_index1] = beta2 * adam_v.em_bi[em_index1] + (1 - beta2) * gt.em_bi[em_index1] * gt.em_bi[em_index1];
                        gt.em_bi[em_index1] = 0.;

                        float m_hat = adam_m.em_bi[em_index1] / (1 - beta1t);
                        float v_hat = adam_v.em_bi[em_index1] / (1 - beta2t);
                        model->em_bi[em_index1] -= alpha * m_hat / ((float)sqrt((float)v_hat) + epsilon);
                    }

                    // bi positional embedding look up table
                    em_index0 = max_bi_positional_em_indexs[2 * batch_j * model->em_dim + 2 * batch_k];
                    em_index1 = max_bi_positional_em_indexs[2 * batch_j * model->em_dim + 2 * batch_k + 1];

                    if (gt.em_bi_pos[em_index0] != 0.)
                    {
                        adam_m.em_bi_pos[em_index0] = beta1 * adam_m.em_bi_pos[em_index0] + (1 - beta1) * gt.em_bi_pos[em_index0];
                        adam_v.em_bi_pos[em_index0] = beta2 * adam_v.em_bi_pos[em_index0] + (1 - beta2) * gt.em_bi_pos[em_index0] * gt.em_bi_pos[em_index0];
                        gt.em_bi_pos[em_index0] = 0.;

                        float m_hat = adam_m.em_bi_pos[em_index0] / (1 - beta1t);
                        float v_hat = adam_v.em_bi_pos[em_index0] / (1 - beta2t);
                        model->em_bi_pos[em_index0] -= alpha * m_hat / ((float)sqrt((float)v_hat) + epsilon);
                    }
                    if (gt.em_bi_pos[em_index1] != 0.)
                    {
                        adam_m.em_bi_pos[em_index1] = beta1 * adam_m.em_bi_pos[em_index1] + (1 - beta1) * gt.em_bi_pos[em_index1];
                        adam_v.em_bi_pos[em_index1] = beta2 * adam_v.em_bi_pos[em_index1] + (1 - beta2) * gt.em_bi_pos[em_index1] * gt.em_bi_pos[em_index1];
                        gt.em_bi_pos[em_index1] = 0.;

                        float m_hat = adam_m.em_bi_pos[em_index1] / (1 - beta1t);
                        float v_hat = adam_v.em_bi_pos[em_index1] / (1 - beta2t);
                        model->em_bi_pos[em_index1] -= alpha * m_hat / ((float)sqrt((float)v_hat) + epsilon);
                    }
                }
            }

            beta1t *= beta1t;
            beta2t *= beta2t;

        } // end_batch
        epoch_end = time(NULL);
        // epoch_end = clock();

        s_loss /= train_data->text_num;
        printf("    loss: %.4f\n", s_loss);
        printf("    time: %lds\n", epoch_end - epoch_start);
        // printf("    time: %.1fs\n", (double)(epoch_end - epoch_start)/CLOCKS_PER_SEC );

        if (vali_data != NULL)
        {
            printf("evaluate vali data...\n");
            evaluate(model, vali_data, batch_size, threads_n);
        }

        printf("\n");

    } //end_epoch
    free(shuffle_index);
    free_model(&adam_m);
    free_model(&adam_v);
    free_model(&gt);
    free(grads_em_ave);
    free(grads_em_pos);
    free(grads_em_bi);
    free(grads_em_bi_pos);
    free(grads_w);
    free(grads_w_bi);
    free(grads_w_positional);
    free(grads_w_bi_positional);
    free(grads_b);
    free(ave_feas);
    free(ave_fea_indexs);
    free(max_bi_feas);
    free(max_bi_fea_indexs);
    free(max_positional_feas);
    free(max_positional_fea_indexs);
    free(max_positional_em_indexs);
    free(max_bi_positional_feas);
    free(max_bi_positional_fea_indexs);
    free(max_bi_positional_em_indexs);
    free(softmax_feas);
    free(losses);
}
void show(int64_t *a, int64_t n)
{
    for (int64_t i = 0; i < n; i++)
        printf("%ld ", a[i]);
    printf("\n");
}

int arg_helper(char *str, int argc, char **argv)
{
    int pos;
    for (pos = 1; pos < argc; pos++)
        if (strcmp(str, argv[pos]) == 0)
            return pos;
    return -1;
}

void save_em(struct model_t *model, char *path, int64_t n)  // TO BE UNDERSTOOD
{
    FILE *fp = NULL;
    fp = fopen(path, "w");
    if (fp == NULL)
    {
        perror("error");
        exit(EXIT_FAILURE);
    }
    for (int64_t i = 0; i < n; i++)
    {
        int64_t pos = i * model->em_dim;
        for (int64_t j = 0; j < model->em_dim; j++)
        {
            if (j == model->em_dim - 1)
            {
                fprintf(fp, "%.8f\n", model->em[pos + j]);
            }
            else
            {
                fprintf(fp, "%.8f ", model->em[pos + j]);
            }
        }
    }
    fclose(fp);
}

int main(int argc, char **argv)
{
    struct model_t model;
    struct dataset_t train_data, vali_data, test_data;

    int64_t em_dim = 200, vocab_num = 0, category_num = 0, em_len = 0, max_text_len = 0;
    int64_t epochs = 10, batch_size = 2000, threads_n = 20;
    float lr = 0.5, limit_vocab=1.;
    char *train_data_path = NULL, *vali_data_path = NULL, *test_data_path = NULL, *em_path = NULL;

    int i;
    if ((i = arg_helper("-dim", argc, argv)) > 0)
        em_dim = (int64_t)atoi(argv[i + 1]);
    if ((i = arg_helper("-vocab", argc, argv)) > 0)
        vocab_num = (int64_t)atoi(argv[i + 1]);
    if ((i = arg_helper("-category", argc, argv)) > 0)
        category_num = (int64_t)atoi(argv[i + 1]);
    if ((i = arg_helper("-epoch", argc, argv)) > 0)
        epochs = (int64_t)atoi(argv[i + 1]);
    if ((i = arg_helper("-batch-size", argc, argv)) > 0)
        batch_size = (int64_t)atoi(argv[i + 1]);
    if ((i = arg_helper("-thread", argc, argv)) > 0)
        threads_n = (int64_t)atoi(argv[i + 1]);
    if ((i = arg_helper("-lr", argc, argv)) > 0)
        lr = (float)atof(argv[i + 1]);
    if ((i = arg_helper("-train", argc, argv)) > 0)
        train_data_path = argv[i + 1];
    if ((i = arg_helper("-vali", argc, argv)) > 0)
        vali_data_path = argv[i + 1];
    if ((i = arg_helper("-test", argc, argv)) > 0)
        test_data_path = argv[i + 1];
    if ((i = arg_helper("-em-path", argc, argv)) > 0)
        em_path = argv[i + 1];
    if ((i = arg_helper("-em-len", argc, argv)) > 0)
        em_len = (int64_t)atoi(argv[i + 1]);
    if ((i = arg_helper("-limit-vocab", argc, argv)) > 0)
        limit_vocab = (float)atof(argv[i + 1]);

    if (vocab_num == 0)
    {
        printf("error: miss -vocab");
        exit(-1);
    }
    if (category_num == 0)
    {
        printf("error: miss -category");
        exit(-1);
    }
    if (train_data_path == NULL)
    {
        printf("error: need train data!");
        exit(-1);
    }

    if (train_data_path != NULL)
        load_data(&train_data, train_data_path, (int64_t)(limit_vocab*vocab_num));
    if (test_data_path != NULL)
        load_data(&test_data, test_data_path, (int64_t)(limit_vocab*vocab_num));
    if (vali_data_path != NULL)
        load_data(&vali_data, vali_data_path, (int64_t)(limit_vocab*vocab_num));

    for (i = 0; i < train_data.text_num; i++)
        if (max_text_len < train_data.text_lens[i])
            max_text_len = train_data.text_lens[i];

    init_model(&model, em_dim, vocab_num, category_num, max_text_len, 1);

    if (vali_data_path != NULL)
        train_adam(&model, &train_data, &vali_data, epochs, batch_size, threads_n);
    else
        train_adam(&model, &train_data, NULL, epochs, batch_size, threads_n);

    if (test_data_path != NULL)
    {
        printf("evaluate test data...\n");
        evaluate(&model, &test_data, batch_size, threads_n);
    }

    if (em_path != NULL)
    {
        printf("saving em...\n");
        if (em_len == 0)
            em_len = model.vocab_num;
        save_em(&model, em_path, em_len);
    }

    free_model(&model);
    if (train_data_path != NULL)
        free_data(&train_data);
    if (test_data_path != NULL)
        free_data(&test_data);
    if (vali_data_path != NULL)
        free_data(&vali_data);

    return 0;
}
