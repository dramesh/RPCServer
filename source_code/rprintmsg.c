/* rprintmsg.c: remote version of "printmsg.c" */  
#include <stdio.h>  
#include <rpc/rpc.h>
#include "msg.h"
#include "timer.c"
#define DEBUG 0

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

int main(int argc, char **argv) {

	int i,j,k,l;
	char *filename;
	CLIENT *cl; 
    int *result;
	char **res;
    char *server; 
    char *message; 
	char *links[1005];
	FILE *file;	
	size_t len = 0;
    ssize_t read;
	long double t_seq,tot_lat=0;
	
	srand(time(NULL));
	stopwatch_init ();
	struct stopwatch_t* timer = stopwatch_create (); assert (timer);
	
	if (argc != 3)  
    { 
        fprintf(stderr, "usage: %s host inputfile\n", argv[0]); 
        exit(1); 
    } 
	
	server = argv[1]; 
	filename = argv[2];
	cl = clnt_create(server, MESSAGEPROG, MESSAGEVERS, "tcp"); 
	if (cl == NULL)  
	{ 
		clnt_pcreateerror (server); 
		exit(1); 
	}
		
	i=0;
	file = fopen (filename, "r");	
	if (file != NULL) {
		char *line; 
		while ((read = getline(&line, &len, file)) != -1) {
			line[strcspn ( line, "\n" )] = '\0';
			
			links[i]=(char*)malloc((strlen(line)+1)*sizeof(char));
			strcpy(links[i],line);
			i++;
			if(DEBUG==1) printf("Request server with url[%d]:%s .\n",i,line);

			stopwatch_start (timer);
			res = handlerequest_1(&line, cl);
			t_seq = stopwatch_stop (timer);
			tot_lat+=t_seq;
			if (res == NULL)  
			{ 
				clnt_perror (cl, server); 
				exit(1); 
			} 
			if(DEBUG==1) printf("Response from server with %d bytes in %Lg secs.\n",strlen(*res),t_seq);
			if(i%100==0){
				Data *data = reportstat_1(NULL,cl);
				printf("%d %.2f %.2Lg\n",data->n_reqs,((float)data->n_hits/(float)data->n_reqs),(long double)(tot_lat/data->n_reqs));
			}
		}
		fclose(file);
	} else {
		printf("Unable to read the input file\n");
		exit (1);
	}

	stopwatch_destroy (timer);
	
	exit (0);
}