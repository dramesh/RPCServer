#include <stdio.h>  
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>
#include <curl/curl.h>
#include <rpc/rpc.h>
#include "msg.h"
#include "timer.c"
#define CACHE_CAPACITY 3000000
#define CACHE_SIZE 150
#define HASHTABLE_SIZE 3000

bool_t
xdr_Data (XDR *xdrs, Data *objp)
{
        register int32_t *buf;

         if (!xdr_int (xdrs, &objp->n_reqs))
                 return FALSE;
         if (!xdr_int (xdrs, &objp->n_hits))
                 return FALSE;
        return TRUE;
}

enum policy{
	LRU=0, 
	LFU=1,
	Rand=2
} ;

struct wd_in {
  size_t size;
  size_t len;
  char *data;
};

struct entry_t {
	char *key;
	int in_cache;
	int cache_index;
	void *queue_pos;
} hashtable[HASHTABLE_SIZE];

struct block_t {
	char *key;
	char *content;
	int block_len;
} cache[CACHE_SIZE];
int n_cache,c_cache;

struct lru_node_t {
	int cache_index;
	struct lru_node_t *next;
};

struct lfu_node_t {
	int cache_index;
	int freq;
	struct lfu_node_t *next;
};

struct lru_node_t *lru_head=NULL;
struct lru_node_t *lru_tail=NULL;

struct lfu_node_t *lfu_head=NULL;
struct lfu_node_t *lfu_tail=NULL;

int n_reqs,n_hits;
enum policy cr_policy = LRU;

unsigned int DJBHash (char* str) {
	
	unsigned int hash = 5381;
	unsigned int i = 0;
	int len = strlen(str);
	for(i=0; i<len; str++, i++)
		hash = ((hash << 5) + hash) + (*str);
	return hash;
}

void init() {

	int i;
	for(i=0; i<HASHTABLE_SIZE; i++) {
		hashtable[i].in_cache=0;
		hashtable[i].cache_index=-1;
	}
	for(i=0; i<CACHE_SIZE; i++) {
		cache[i].block_len=0;
	}
}

/* This function is registered as a callback with CURL.  As the data
   from the requested webpage is returned in chunks, write_data is
   called with each chunk.  */
static size_t write_data(void *buffer, size_t size, 
                         size_t nmemb, void *userp) {
  struct wd_in *wdi = userp;

  while(wdi->len + (size * nmemb) >= wdi->size) {
    /* check for realloc failing in real code. */
    wdi->data = realloc(wdi->data, wdi->size*2);
    wdi->size*=2;
  }

  memcpy(wdi->data + wdi->len, buffer, size * nmemb);
  wdi->len+=size*nmemb;

  return size * nmemb;
}

 /* Remote version of "printmessage" */  

char **httpGet(char **msg)  
{ 
    
  CURL *curl;
  CURLcode res;
  struct wd_in wdi;
  char **result;

  memset(&wdi, 0, sizeof(wdi));

  /* Get a curl handle.  Each thread will need a unique handle. */
  curl = curl_easy_init();

  if(NULL != curl) {
    wdi.size = 1024;
    /* Check for malloc failure in real code. */
    wdi.data = malloc(wdi.size);

    /* Set the URL for the operation. */
    curl_easy_setopt(curl, CURLOPT_URL, *msg);

    /* "write_data" function to call with returned data. */
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);

    /* userp parameter passed to write_data. */
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wdi);

    /* Actually perform the query. */
    res = curl_easy_perform(curl);

    /* Check the return value and do whatever. */

    /* Clean up after ourselves. */
    curl_easy_cleanup(curl);
  }
  else {
    fprintf(stderr, "Error: could not get CURL handle.\n");
    return (NULL);
  }

  /* Now wdi.data has the data from the GET and wdi.len is the length
     of the data available, so do whatever. */
  
  result=(char**)malloc(sizeof(char*));
  *result=(char*)malloc(wdi.len*sizeof(char));
  
  memcpy(*result,wdi.data,wdi.len);
  return (result); 
} 

int lru_cachehit(int hash_index){

	struct lru_node_t *temp,*tmpnext;
	unsigned int h1,h2;
	int i1,i2;
	temp=(struct lru_node_t *)hashtable[hash_index].queue_pos;
	if(lru_tail==temp)
		return;	//node at right pos already; nothing to do
	if(lru_head==temp) {
		lru_head=lru_head->next;
		lru_tail->next=temp;
		lru_tail=temp;
	} else if(temp->next==lru_tail){
		h1=DJBHash(cache[temp->cache_index].key)&(HASHTABLE_SIZE-1);
		h2=DJBHash(cache[lru_tail->cache_index].key)&(HASHTABLE_SIZE-1);
		i1=temp->cache_index;
		temp->cache_index=lru_tail->cache_index;
		lru_tail->cache_index=i1;
		hashtable[h1].queue_pos=lru_tail;
		hashtable[h2].queue_pos=temp;
	} else {
		tmpnext=temp->next;
		h1=DJBHash(cache[temp->cache_index].key)&(HASHTABLE_SIZE-1);
		h2=DJBHash(cache[tmpnext->cache_index].key)&(HASHTABLE_SIZE-1);
		temp->next=tmpnext->next;
		i1=temp->cache_index;
		temp->cache_index=tmpnext->cache_index;
		lru_tail->next=tmpnext;
		tmpnext->cache_index=i1;
		tmpnext->next=NULL;
		lru_tail=tmpnext;	
		hashtable[h1].queue_pos=lru_tail;
		hashtable[h2].queue_pos=temp;
	}
}

int lru_evict(int hint){

	int index=-1;
	struct lru_node_t *todel,*tmpnext;
	unsigned int h1;
	if(hint==-1) {
		todel=lru_head;
		if(todel!=NULL) {
			index=todel->cache_index;
			unsigned int todel_hash = DJBHash(cache[index].key)&(HASHTABLE_SIZE-1);
			hashtable[todel_hash].cache_index = -1;
			hashtable[todel_hash].in_cache = 0;
			hashtable[todel_hash].queue_pos = NULL;
			lru_head=lru_head->next;
			if(lru_head==NULL)
				lru_tail=NULL;
			n_cache--;
			c_cache-=cache[index].block_len;
			cache[index].block_len=0;
			if(cache[index].key!=NULL)		{free(cache[index].key);			cache[index].key=NULL;}
			if(cache[index].content!=NULL)	{free(cache[index].content); 		cache[index].content=NULL;}
			if(todel!=NULL)					{free(todel);						todel=NULL;}
		}
	} else {
		unsigned int todel_hash = DJBHash(cache[hint].key)&(HASHTABLE_SIZE-1);
		todel=hashtable[todel_hash].queue_pos;
		if(todel!=NULL){
			if(todel==lru_tail){
				tmpnext=lru_head;
				while(tmpnext->next!=lru_tail)
					tmpnext=tmpnext->next;
				if(tmpnext->next==lru_tail){
					index=lru_tail->cache_index;
					if(cache[hint].key!=NULL)		{free(cache[hint].key); 		cache[hint].key=NULL;}
					if(cache[hint].content!=NULL)	{free(cache[hint].content); 	cache[hint].content=NULL;}
					if(lru_tail!=NULL)				{free(lru_tail);				lru_tail=NULL;}
					lru_tail=tmpnext;
					lru_tail->next=NULL;
					h1=DJBHash(cache[tmpnext->cache_index].key)&(HASHTABLE_SIZE-1);
					hashtable[h1].queue_pos=lru_tail;
					n_cache--;
					c_cache-=cache[hint].block_len;
					cache[hint].block_len=0;
				}
			}else {
				tmpnext=todel->next;
				index=tmpnext->cache_index;
				h1= DJBHash(cache[index].key)&(HASHTABLE_SIZE-1);
				todel->cache_index=index;
				todel->next=tmpnext->next;
				if(tmpnext==lru_tail)
					lru_tail=todel;
				hashtable[h1].queue_pos=todel;
				if(cache[hint].key!=NULL)		{free(cache[hint].key); 		cache[hint].key=NULL;}
				if(cache[hint].content!=NULL)	{free(cache[hint].content); 	cache[hint].content=NULL;}
				if(tmpnext!=NULL)				{free(tmpnext);					tmpnext=NULL;}
				n_cache--;
				c_cache-=cache[hint].block_len;
				cache[hint].block_len=0;
			}
			hashtable[todel_hash].in_cache = 0;
			hashtable[todel_hash].cache_index = -1;
			hashtable[todel_hash].queue_pos = NULL;
		}
		index=hint;
	}
	int rep=n_cache;
	if(rep<0) return index;
	while(cache[rep].block_len==0 && rep>0)
		rep--;
	if(cache[rep].block_len>0 && index!=rep) {
		h1= DJBHash(cache[rep].key)&(HASHTABLE_SIZE-1);
		cache[index].key=cache[rep].key;
		cache[rep].key=NULL;
		cache[index].content=cache[rep].content;
		cache[rep].content=NULL;
		cache[index].block_len=cache[rep].block_len;
		cache[rep].block_len=0;
		hashtable[h1].cache_index=index;
		struct lru_node_t *qn = (struct lru_node_t *)hashtable[h1].queue_pos;
		qn->cache_index=index;
		index=rep;
	}
	return index;
}

int lfu_cachehit(int hash_index){

	struct lfu_node_t *temp,*tmpnext, *ptr;
	unsigned int h1,h2;
	int i1,i2,f;
	temp=(struct lfu_node_t *)hashtable[hash_index].queue_pos;
	temp->freq++; //increment frequency
	
	if(lfu_tail==temp)
		return;	//node at right pos already; nothing to do 
	
	ptr = temp;
	while(ptr->next!=NULL && temp->freq >= ptr->next->freq) {
		ptr = ptr->next;
	}
	
	if (temp != ptr) {
		if(lfu_head==temp) {
			lfu_head=lfu_head->next;
			temp->next=ptr->next;
			ptr->next=temp;
			if (lfu_tail==ptr) lfu_tail = temp;
		} else if(temp->next==ptr){
			h1=DJBHash(cache[temp->cache_index].key)&(HASHTABLE_SIZE-1);
			h2=DJBHash(cache[ptr->cache_index].key)&(HASHTABLE_SIZE-1);
			i1=temp->cache_index;
			temp->cache_index=ptr->cache_index;
			ptr->cache_index=i1;
			f=temp->freq;
			temp->freq=ptr->freq;
			ptr->freq=f;
			hashtable[h1].queue_pos=ptr;
			hashtable[h2].queue_pos=temp;
		} else {
			tmpnext=temp->next;
			h1=DJBHash(cache[temp->cache_index].key)&(HASHTABLE_SIZE-1);
			h2=DJBHash(cache[tmpnext->cache_index].key)&(HASHTABLE_SIZE-1);
			temp->next=tmpnext->next;
			i1=temp->cache_index;
			temp->cache_index=tmpnext->cache_index;
			tmpnext->cache_index=i1;
			f=temp->freq;
			temp->freq=tmpnext->freq;
			tmpnext->freq=f;
			tmpnext->next=ptr->next;
			ptr->next=tmpnext;	
			hashtable[h1].queue_pos=tmpnext;
			hashtable[h2].queue_pos=temp;
			if (lfu_tail==ptr) lfu_tail = tmpnext;
		}
		
	}
	
}

int lfu_evict(int hint){

	int index=-1,freq=1;
	struct lfu_node_t *todel,*tmpnext;
	unsigned int h1;
	unsigned int todel_hash;
	if(hint!=-1) {
		todel_hash= DJBHash(cache[hint].key)&(HASHTABLE_SIZE-1);
		todel=hashtable[todel_hash].queue_pos;
	}
	
	if(hint==-1 || todel==lfu_head) {
		todel=lfu_head;
		if(todel!=NULL) {
			index=todel->cache_index;
			todel_hash = DJBHash(cache[index].key)&(HASHTABLE_SIZE-1);
			hashtable[todel_hash].cache_index = -1;
			hashtable[todel_hash].in_cache = 0;
			hashtable[todel_hash].queue_pos = NULL;
			lfu_head=lfu_head->next;
			if(lfu_head==NULL)
				lfu_tail=NULL;
			n_cache--;
			c_cache-=cache[index].block_len;
			cache[index].block_len=0;
			if(cache[index].key!=NULL)		{free(cache[index].key);			cache[index].key=NULL;}
			if(cache[index].content!=NULL)	{free(cache[index].content); 		cache[index].content=NULL;}
			if(todel!=NULL)					{free(todel);						todel=NULL;}
		}
	} else {
		
		if(todel!=NULL){
			if(todel==lfu_tail){
				tmpnext=lfu_head;
				while(tmpnext->next!=lfu_tail)
					tmpnext=tmpnext->next;
				if(tmpnext->next==lfu_tail){
					index=lfu_tail->cache_index;
					if(cache[hint].key!=NULL)		{free(cache[hint].key); 		cache[hint].key=NULL;}
					if(cache[hint].content!=NULL)	{free(cache[hint].content); 	cache[hint].content=NULL;}
					if(lfu_tail!=NULL)				{free(lfu_tail);				lfu_tail=NULL;}
					lfu_tail=tmpnext;
					lfu_tail->next=NULL;
					h1=DJBHash(cache[tmpnext->cache_index].key)&(HASHTABLE_SIZE-1);
					hashtable[h1].queue_pos=lfu_tail;
					n_cache--;
					c_cache-=cache[hint].block_len;
					cache[hint].block_len=0;
				} else {
					printf("error no evict");
					exit(1);
				}
			}else {
				tmpnext=todel->next;
				index=tmpnext->cache_index;
				freq=tmpnext->freq;
				h1= DJBHash(cache[index].key)&(HASHTABLE_SIZE-1);
				todel->cache_index=index;
				todel->freq=freq;
				todel->next=tmpnext->next;
				if(tmpnext==lfu_tail)
					lfu_tail=todel;
				hashtable[h1].queue_pos=todel;
				if(cache[hint].key!=NULL)		{free(cache[hint].key); 		cache[hint].key=NULL;}
				if(cache[hint].content!=NULL)	{free(cache[hint].content); 	cache[hint].content=NULL;}
				if(tmpnext!=NULL)				{free(tmpnext);					tmpnext=NULL;}
				n_cache--;
				c_cache-=cache[hint].block_len;
				cache[hint].block_len=0;
			}
			hashtable[todel_hash].in_cache = 0;
			hashtable[todel_hash].cache_index = -1;
			hashtable[todel_hash].queue_pos = NULL;
		}
		index=hint;
	}
	int rep=n_cache;
	if(rep<0) return index;
	while(cache[rep].block_len==0 && rep>0)
		rep--;
	if(cache[rep].block_len>0 && index!=rep) {
		h1= DJBHash(cache[rep].key)&(HASHTABLE_SIZE-1);
		cache[index].key=cache[rep].key;
		cache[rep].key=NULL;
		cache[index].content=cache[rep].content;
		cache[rep].content=NULL;
		cache[index].block_len=cache[rep].block_len;
		cache[rep].block_len=0;
		hashtable[h1].cache_index=index;
		struct lfu_node_t *qn = (struct lfu_node_t *)hashtable[h1].queue_pos;
		qn->cache_index=index;
		index=rep;
	}
	if(index==-1) {
		printf("Unable to locate a free cache block\n");
		exit(1);
	}
	return index;
}

int rand_evict(int hint){
	int evict_index = rand() % (n_cache - 1);
	unsigned int h1;
	while (cache[evict_index].key == NULL) {
		evict_index = rand() % (n_cache - 1);
	}
	unsigned int todel_hash = DJBHash(cache[evict_index].key)&(HASHTABLE_SIZE-1);
	hashtable[todel_hash].cache_index = -1;
	hashtable[todel_hash].in_cache = 0;
	hashtable[todel_hash].queue_pos = NULL;
	n_cache--;
	c_cache-=cache[evict_index].block_len;
	cache[evict_index].block_len=0;
	if(cache[evict_index].key!=NULL)		{free(cache[evict_index].key);			cache[evict_index].key=NULL;}
	if(cache[evict_index].content!=NULL)	{free(cache[evict_index].content); 		cache[evict_index].content=NULL;}
	
	int rep=n_cache;
	if(rep<0) return evict_index;
	while(cache[rep].block_len==0 && rep>0)
		rep--;
	if(cache[rep].block_len>0 && evict_index!=rep) {
		h1= DJBHash(cache[rep].key)&(HASHTABLE_SIZE-1);
		cache[evict_index].key=cache[rep].key;
		cache[rep].key=NULL;
		cache[evict_index].content=cache[rep].content;
		cache[rep].content=NULL;
		cache[evict_index].block_len=cache[rep].block_len;
		cache[rep].block_len=0;
		hashtable[h1].cache_index=evict_index;
		evict_index=rep;
	}
	return evict_index;
}

char **handlerequest_1_svc(char **msg, struct svc_req *rqstp){

	int i,evict_index;	
	for(i=n_cache;i<CACHE_SIZE&&cache[i].key!=NULL;i++);
	if(i>=CACHE_SIZE)	for(i=0;i<CACHE_SIZE&&cache[i].key!=NULL;i++);
	if(i>=CACHE_SIZE) { printf("cache limit!!!!!!\n"); }
	else evict_index=i;
	char *test = *msg;
	int test_len=(strlen(test)*sizeof(char))+1;
	int content_len=0;
	unsigned int code = (DJBHash(test))&(HASHTABLE_SIZE-1);
	char **content=(char**)malloc(sizeof(char*));
	n_reqs++;
	
	if(hashtable[code].in_cache==1 && strcmp(test,hashtable[code].key)==0) {
		//hashtable entry found
		
		switch(cr_policy){
			case LRU:
				lru_cachehit(code);
				break;
			case LFU:
				lfu_cachehit(code);
				break;
			default:
				break;
		}
		
		n_hits++;
		content=&cache[hashtable[code].cache_index].content;
	} 
	else {
		
		/* Fetch HTTP page here */
		content=httpGet(msg);
		content_len=(strlen(*content)*sizeof(char))+1;
		if(content_len<=1){
			printf("ERROR: The HTTP response for the specified URL is null.\n");
			return content;
		}
		
		if(hashtable[code].in_cache==1){
			//hash collision, evict existing entry
			switch(cr_policy){
				case LFU:
					evict_index=lfu_evict(hashtable[code].cache_index);
					break;
				case Rand:
					evict_index=rand_evict(hashtable[code].cache_index);
					break;
				case LRU:
				default:
					evict_index=lru_evict(hashtable[code].cache_index);
					break;
			}
			
		}
		else if((test_len+content_len) >= CACHE_CAPACITY) {
			printf("WARN:Request size greater than cache capacity. Skip caching.\n");
			return content;
		}
		else if((c_cache+test_len+content_len) >= CACHE_CAPACITY || n_cache >= CACHE_SIZE) {
			//evict until the no_of_free_cache_blocks is less than no_of_total_cache_blocks and
			// the size_of_used_cache is less than total_cache_size
			while((c_cache+test_len+content_len) >= CACHE_CAPACITY || n_cache >= CACHE_SIZE) {
				switch(cr_policy){
					case LFU:
						evict_index = lfu_evict(-1);
						break;
					case Rand:
						evict_index=rand_evict(-1);
						break;
					case LRU:
					default:
						evict_index = lru_evict(-1);
						break;
				}
			}
		}
		hashtable[code].key=malloc(test_len);
		strcpy(hashtable[code].key,test);
		hashtable[code].in_cache=1;
		hashtable[code].cache_index=evict_index;
		cache[evict_index].key=malloc(test_len);
		strcpy(cache[evict_index].key,test);
		cache[evict_index].content=malloc(content_len);
		strcpy(cache[evict_index].content,*content);
		struct lru_node_t *temp1 = malloc(sizeof(struct lru_node_t));
		struct lfu_node_t *temp = malloc(sizeof(struct lfu_node_t));
					
		switch(cr_policy){
				case LRU:
					temp1->cache_index = hashtable[code].cache_index;
					temp1->next = NULL;
					hashtable[code].queue_pos = temp1;
					if(lru_head==NULL) {
						lru_head = temp1;
						lru_tail = temp1;
					} else {
						lru_tail->next = temp1;
						lru_tail = temp1;
					}
					break;
				case LFU:
					temp->cache_index = hashtable[code].cache_index;
					temp->next = NULL;
					temp->freq=1;
					hashtable[code].queue_pos = temp;
					if(lfu_head==NULL) {
						lfu_head = temp;
						lfu_tail = temp;
					} else {
						temp->next=lfu_head;
						lfu_head = temp;
					}
					break;
				default:
					break;
		}
		
		n_cache++;
		cache[evict_index].block_len=test_len+content_len;
		c_cache+=cache[evict_index].block_len;
	}
	return content;
}

Data *reportstat_1_svc(void *t, struct svc_req *rqstp){

	static Data data;
	data.n_reqs=n_reqs;
	data.n_hits=n_hits;
	return &data;
	
}
