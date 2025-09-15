#include <stdio.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <curl/curl.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <sys/queue.h>
#include "./cat_png_functions/zutil.h"
#include "./cat_png_functions/crc.h"
#include "./cat_png_functions/pnginfo.h"

struct thread_arg
{
    char url[256];
};

#define ECE252_HEADER "X-Ece252-Fragment: "
#define BUF_SIZE 1048576 /* 1024*1024 = 1M */
#define BUF_INC 524288   /* 1024*512  = 0.5M */

#define max(a, b) \
    ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

typedef struct recv_buf2
{
    char *buf;       /* memory to hold a copy of received data */
    size_t size;     /* size of valid data in buf in bytes*/
    size_t max_size; /* max capacity of buf in bytes*/
    int seq;         /* >=0 sequence number extracted from http header */
                     /* <0 indicates an invalid seq number */
} RECV_BUF;

size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata);
size_t write_cb_curl3(char *p_recv, size_t size, size_t nmemb, void *p_userdata);
int recv_buf_init(RECV_BUF *ptr, size_t max_size);
int recv_buf_cleanup(RECV_BUF *ptr);
int write_file(const char *path, const void *in, size_t len);

/**
 * @brief  cURL header call back function to extract image sequence number from
 *         http header data. An example header for image part n (assume n = 2) is:
 *         X-Ece252-Fragment: 2
 * @param  char *p_recv: header data delivered by cURL
 * @param  size_t size size of each memb
 * @param  size_t nmemb number of memb
 * @param  void *userdata user defined data structurea
 * @return size of header data received.
 * @details this routine will be invoked multiple times by the libcurl until the full
 * header data are received.  we are only interested in the ECE252_HEADER line
 * received so that we can extract the image sequence number from it. This
 * explains the if block in the code.
 */
size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata)
{
    int realsize = size * nmemb;
    RECV_BUF *p = userdata;

    if (realsize > strlen(ECE252_HEADER) &&
        strncmp(p_recv, ECE252_HEADER, strlen(ECE252_HEADER)) == 0)
    {

        /* extract img sequence number */
        p->seq = atoi(p_recv + strlen(ECE252_HEADER));
    }
    return realsize;
}

/**
 * @brief write callback function to save a copy of received data in RAM.
 *        The received libcurl data are pointed by p_recv,
 *        which is provided by libcurl and is not user allocated memory.
 *        The user allocated memory is at p_userdata. One needs to
 *        cast it to the proper struct to make good use of it.
 *        This function maybe invoked more than once by one invokation of
 *        curl_easy_perform().
 */

size_t write_cb_curl3(char *p_recv, size_t size, size_t nmemb, void *p_userdata)
{
    size_t realsize = size * nmemb;
    RECV_BUF *p = (RECV_BUF *)p_userdata;

    if (p->size + realsize + 1 > p->max_size)
    { /* hope this rarely happens */
        /* received data is not 0 terminated, add one byte for terminating 0 */
        size_t new_size = p->max_size + max(BUF_INC, realsize + 1);
        char *q = realloc(p->buf, new_size);
        if (q == NULL)
        {
            perror("realloc"); /* out of memory */
            return -1;
        }
        p->buf = q;
        p->max_size = new_size;
    }

    memcpy(p->buf + p->size, p_recv, realsize); /*copy data from libcurl*/
    p->size += realsize;
    p->buf[p->size] = 0;

    return realsize;
}

int recv_buf_init(RECV_BUF *ptr, size_t max_size)
{
    void *p = NULL;

    if (ptr == NULL)
    {
        return 1;
    }

    p = malloc(max_size);
    if (p == NULL)
    {
        return 2;
    }

    ptr->buf = p;
    ptr->size = 0;
    ptr->max_size = max_size;
    ptr->seq = -1; /* valid seq should be non-negative */
    return 0;
}

int recv_buf_cleanup(RECV_BUF *ptr)
{
    if (ptr == NULL)
    {
        return 1;
    }

    free(ptr->buf);
    ptr->size = 0;
    ptr->max_size = 0;
    return 0;
}

typedef struct img_data
{
    unsigned char buf[10000]; // image segment is less than 10000 bytes
    size_t size;
    int seq;
} img_data;

typedef struct ring_buff
{
    int prod_it;
    int cons_it;
    img_data *img_data;
    int new_img_count;
} ring_buff;

typedef struct shared
{
    sem_t spaces;
    sem_t items;
    pthread_mutex_t lock;
    unsigned char buffer[300 * (400 * 4 + 1)];
    unsigned long total_IDAT_compress_length;
    int images_downloaded;
    int images_processed;
} shared;

void producer(shared *shared_mem, ring_buff *placeholder, int N, int B)
{
    while (1)
    {
        pthread_mutex_lock(&shared_mem->lock);
        int img_sec = shared_mem->images_downloaded; // check how many images have been downloaded
        shared_mem->images_downloaded += 1;
        pthread_mutex_unlock(&shared_mem->lock);
        if (img_sec >= 50) // break out of loop if 50+ have been downloaded
        {
            break;
        }
        else // wait for space to show up in buffer to download
        {
            sem_wait(&shared_mem->spaces);
        }
        CURL *curl_handle;
        CURLcode res;
        RECV_BUF recv_buf;
        char url[256];

        recv_buf_init(&recv_buf, BUF_SIZE);

        /* init a curl session */
        curl_handle = curl_easy_init();

        if (curl_handle == NULL)
        {
            fprintf(stderr, "curl_easy_init: returned NULL\n");
        }
        else
        {
            /* specify URL to get */
            sprintf(url, "http://ece252-%d.uwaterloo.ca:2530/image?img=%d&part=%d", img_sec % 3 + 1, N, img_sec);
            curl_easy_setopt(curl_handle, CURLOPT_URL, url);

            // set DNS cache
            curl_easy_setopt(curl_handle, CURLOPT_DNS_CACHE_TIMEOUT, 60L); // Cache DNS for 60 seconds

            /* register write call back function to process received data */
            curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_cb_curl3);
            /* user defined data structure passed to the call back function */
            curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&recv_buf);

            /* register header call back function to process received header data */
            curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, header_cb_curl);
            /* user defined data structure passed to the call back function */
            curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, (void *)&recv_buf);

            /* some servers requires a user-agent field */
            curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");

            /* get it! */
            res = curl_easy_perform(curl_handle);

            // get curl again if couldn't rersolve host (common error when running high thread count)
            while (res == CURLE_COULDNT_RESOLVE_HOST)
            {
                res = curl_easy_perform(curl_handle);
            }

            if (res != CURLE_OK)
            {
                fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            }
            else
            {
                pthread_mutex_lock(&shared_mem->lock);
                if (placeholder->prod_it == B)
                { // buffer cap reached, reset iterator to 0
                    placeholder->prod_it = 0;
                }
                img_data *temp = placeholder->img_data + placeholder->prod_it; // save data at current iterator of ring buffer
                placeholder->prod_it += 1;                                     // increase iterator for next
                temp->seq = recv_buf.seq;                                      // store img sequence number
                memcpy(temp->buf, recv_buf.buf, recv_buf.size);                // store img data
                temp->size = recv_buf.size;                                    // store size (for memcpy and stuff)
                // printf("image %d downloaded\n", temp->seq);
                placeholder->new_img_count += 1; // increase new_img count for consumers
                // printf("prod img_count: %d\n", placeholder->new_img_count);
                pthread_mutex_unlock(&shared_mem->lock);
                sem_post(&shared_mem->items); // signal there is an image to download
            }
        }
        /* cleaning up */
        curl_easy_cleanup(curl_handle);
        recv_buf_cleanup(&recv_buf);
    }
}

void consumer(shared *shared_mem, ring_buff *placeholder, int x, int B)
{
    while (1)
    {
        pthread_mutex_lock(&shared_mem->lock);
        int img_sec = shared_mem->images_processed; // how many current images have been processed
        shared_mem->images_processed += 1;          // increment global counter
        pthread_mutex_unlock(&shared_mem->lock);
        if (img_sec >= 50) // break out if 50
        {
            break;
        }
        else // wait for new images to come in
        {
            sem_wait(&shared_mem->items);
        }
        pthread_mutex_lock(&shared_mem->lock);
        if (placeholder->cons_it == B) // check iterator, if it's at 50 (from prev), reset to 0
        {
            placeholder->cons_it = 0;
        }
        img_data *temp = placeholder->img_data + placeholder->cons_it; // image data with the current iterator to be read from
        placeholder->cons_it += 1;                                     // increment iterator
        char *pic = malloc(temp->size);
        memcpy(pic, temp->buf, temp->size); // read picture
        placeholder->new_img_count -= 1;
        // printf("cons img_count: %d\n", placeholder->new_img_count);
        pthread_mutex_unlock(&shared_mem->lock);
        
        usleep(x * 1000); // sleep in microseconds, *1000 for milli

        // inflate img data
        unsigned int curr_height = 6;
        unsigned int width = 400;
        unsigned int data_length;
        memcpy(&data_length, pic + 33, 4); // read compressed data length
        data_length = htonl(data_length);
        unsigned char *IDAT_Buf = malloc(data_length); // buffer for holding IDAT chunk
        memcpy(IDAT_Buf, pic + 41, data_length);       // read idat info
        unsigned long decompressed_bytes = (curr_height * (width * 4 + 1));
        unsigned char *uncompressed_buff = malloc(decompressed_bytes); // for holding decompressed data
        mem_inf(uncompressed_buff, &decompressed_bytes, IDAT_Buf, data_length);
        pthread_mutex_lock(&shared_mem->lock); // write decompressed IDAT to big buffer after locking
        memcpy(shared_mem->buffer + (decompressed_bytes * temp->seq), uncompressed_buff, decompressed_bytes);
        shared_mem->total_IDAT_compress_length += data_length;
        // printf("inflated img %d to big buff\n", temp->seq);
        pthread_mutex_unlock(&shared_mem->lock);
        if (placeholder->new_img_count < B) // if new images have been read, tell producer to give more
        {
            sem_post(&shared_mem->spaces);
        }

        // free mallocs
        free(IDAT_Buf);
        free(uncompressed_buff);
        free(pic);
    }
}

int main(int argc, char **argv)
{
    int B = atoi(argv[1]); // buffer size
    int P = atoi(argv[2]); // producers
    int C = atoi(argv[3]); // consumers
    int X = atoi(argv[4]); // consumer sleep time
    int N = atoi(argv[5]); // image #

    pid_t cpids[P + C];
    pid_t pid = 0;

    int shmid = shmget(IPC_PRIVATE, sizeof(struct shared), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    if (shmid == -1)
    {
        perror("shmget");
    }
    void *share_at = shmat(shmid, NULL, 0);
    if (share_at == (void *)-1)
    {
        perror("shmat");
        abort();
    }
    shared *share = (shared *)share_at;
    share->total_IDAT_compress_length = 0;
    share->images_downloaded = 0;
    share->images_processed = 0;

    // CIRCLEQ_HEAD(ring_buffer, recv_chunk) head;
    // need to track max and current size - struct?

    int shmid_ring = shmget(IPC_PRIVATE, sizeof(ring_buff) + B * sizeof(img_data), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    if (shmid_ring == -1)
    {
        perror("shmget");
    }
    void *start_ring = shmat(shmid_ring, NULL, 0);
    if (start_ring == (void *)-1)
    {
        perror("shmat");
        abort();
    }

    // initialize ring buffer
    ring_buff *shared_ring = (ring_buff *)start_ring;
    shared_ring->new_img_count = 0;
    shared_ring->cons_it = 0;
    shared_ring->prod_it = 0;
    shared_ring->img_data = (img_data *)(start_ring + sizeof(ring_buff)); // set address after shared struct
    for (int i = 0; i < B; i++)
    {
        img_data *temp = shared_ring->img_data + i;
        temp->seq = -1;
        temp->size = 0;
    }

    // if time add error check for everything below
    if (sem_init(&share->spaces, 1, B) != 0)
    {
        perror("sem_init");
        abort();
    }; // 1 = shared between processes should be 1, B;
    if (sem_init(&share->items, 1, 0) != 0)
    {
        perror("sem_init");
        abort();
    };

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&share->lock, &attr);

    double times[2];
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0)
    {
        perror("gettimeofday");
        abort();
    }
    times[0] = (tv.tv_sec) + tv.tv_usec / 1000000.;

    for (int i = 0; i < P; i++)
    {
        // pid[i] = fork();
        pid = fork();
        if (pid > 0)
        {
            cpids[i] = pid;
        }
        else if (pid == 0)
        {
            producer(share, shared_ring, N, B);
            // shmdt(share_at);
            // shmdt(start_ring);
            exit(0);
        }
        else
        {
            perror("fork");
            abort();
        }
    }

    // consumers:
    for (int i = P; i < P + C; i++)
    {
        pid = fork();
        if (pid > 0)
        {
            cpids[i] = pid;
        }
        else if (pid == 0)
        {
            consumer(share, shared_ring, X, B);
            // shmdt(share_at);
            // shmdt(start_ring);
            exit(0);
        }
        else
        {
            perror("fork");
            abort();
        }
    }

    int state;

    // parent waits for prod and consumer to be done
    if (pid > 0)
    {
        for (int i = 0; i < P; i++)
        {
            waitpid(cpids[i], &state, 0);
        }

        for (int i = P; i < P + C; i++)
        {
            waitpid(cpids[i], &state, 0);
        }

        // concatenate image after grabbing all segments
        unsigned char header[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}; // png header

        FILE *pngptr = fopen("all.png", "wb"); // create all.png file
        fwrite(header, 8, 1, pngptr);          // write header
        int IHDR_size = htonl(13);
        fwrite(&IHDR_size, 4, 1, pngptr); // put in length
        fwrite("IHDR", 4, 1, pngptr);     // write type
        unsigned int new_height = htonl(300);
        unsigned int new_width = htonl(400);
        fwrite(&new_width, 4, 1, pngptr); // put in height and width
        fwrite(&new_height, 4, 1, pngptr);
        unsigned char IHDR_Dat[5] = {8, 6, 0, 0, 0}; // rest of IHDR data
        fwrite(IHDR_Dat, 5, 1, pngptr);
        unsigned char IHDR_Buf[17]; // buffer for calculating crc
        memcpy(IHDR_Buf, "IHDR", 4);
        memcpy(IHDR_Buf + 4, &new_width, 4);
        memcpy(IHDR_Buf + 8, &new_height, 4);
        memcpy(IHDR_Buf + 12, IHDR_Dat, 5);
        unsigned long IHDR_crc = htonl(crc(IHDR_Buf, 17)); // crc calculation
        fwrite(&IHDR_crc, 4, 1, pngptr);                   // write crc

        unsigned char *IDAT_Def = malloc(share->total_IDAT_compress_length);
        unsigned long temp_length = share->total_IDAT_compress_length;
        // char temp_buff[300 * (400 * 4 + 1)];
        // memcpy(temp_buff, share->buffer)
        mem_def(IDAT_Def, &temp_length, share->buffer, (300 * (400 * 4 + 1)), -1); // default compression

        // Writing IDAT
        unsigned long IDAT_length = htonl(temp_length);
        fwrite(&IDAT_length, 4, 1, pngptr);                             // write length of IDAT
        fwrite("IDAT", 4, 1, pngptr);                                   // write IDAT type
        fwrite(IDAT_Def, temp_length, 1, pngptr); // write the compressed IDAT data
        unsigned char IDAT_Buf[temp_length + 4];  // for crc calculations
        memcpy(IDAT_Buf, "IDAT", 4);                                    // copy type and data into buffer
        memcpy(IDAT_Buf + 4, IDAT_Def, temp_length);
        unsigned int IDAT_crc = htonl(crc(IDAT_Buf, (temp_length + 4)));
        fwrite(&IDAT_crc, 4, 1, pngptr); // write crc
        free(IDAT_Def);

        int IEND_len = htonl(0); // length of IEND
        fwrite(&IEND_len, 4, 1, pngptr);
        fwrite("IEND", 4, 1, pngptr); // write type of IEND
        unsigned char IEND_Buf[4];
        memcpy(IEND_Buf, "IEND", 4); // for CRC calculation
        unsigned int IEND_crc = htonl(crc(IEND_Buf, 4));
        fwrite(&IEND_crc, 4, 1, pngptr);
        fclose(pngptr);

        if (gettimeofday(&tv, NULL) != 0)
        {
            perror("gettimeofday");
            abort();
        }
        times[1] = (tv.tv_sec) + tv.tv_usec / 1000000.;
        printf("paster2 execution time: %.6lf seconds\n", times[1] - times[0]);

        sem_destroy(&share->spaces);
        sem_destroy(&share->items);
        pthread_mutexattr_destroy(&attr);
        pthread_mutex_destroy(&share->lock);
        if (shmdt(share_at) != 0)
        {
            perror("shmdt");
            abort();
        }
        if (shmctl(shmid, IPC_RMID, NULL) == -1)
        {
            perror("shmctl");
            abort();
        }
        if (shmdt(start_ring) != 0)
        {
            perror("shmdt");
            abort();
        }
        if (shmctl(shmid_ring, IPC_RMID, NULL) == -1)
        {
            perror("shmctl");
            abort();
        }
    }
    return 0;
}