#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100
#define MAX_CLIENTS 100

#define MSG_INIT			1   // Client sends to tracker: initial files (for which they are seeds)
#define MSG_INIT_ACK		2   // Tracker sends to client: confirmation receipt of all files
#define MSG_REQUEST_SWARM	3   // Client sends to tracker: request swarm of a file (hashes and seeds list)
#define MSG_REQUEST_SEEDS	4   // CLient sends to tracker: request seeds list only
#define MSG_SWARM_INFO		5   // Tracker sends to client: the swarm file
#define MSG_REQUEST_SEG		6   // Client sends to seed/peer: request a segment
#define MSG_ACK_SEG			7   // Seed/Peer sends to client: ack with the segment
#define MSG_FINISH_FILE		8   // Client sends to tracker: confirm file downloaded
#define MSG_FINISH_ALL		9   // Client sends to tracker: confirm all files downloaded
#define MSG_TERMINATE		10  // Tracker sends to client: Finish program

typedef struct {
	char name[MAX_FILENAME + 1];
	int  total_chunks;
	char chunks[MAX_CHUNKS][HASH_SIZE + 1];
} file_t;

typedef struct {
	char name[MAX_FILENAME + 1];
	int  total_chunks;
	char seg_hashes[MAX_CHUNKS][HASH_SIZE + 1];
	int  have_chunk[MAX_CHUNKS];
} wanted_file_t;

typedef struct client_t {
	int rank;
	int num_owned;
	file_t owned[MAX_FILES];

	int num_wanted;
	wanted_file_t wanted[MAX_FILES];
	int finished_all;  // 1 when a the client has finished to download all its files
} client_t;

/* Struct tracker */
typedef struct {
	char file_name[MAX_FILENAME + 1];
	int  total_chunks;
	char chunks[MAX_CHUNKS][HASH_SIZE + 1];
	int  swarm[MAX_CLIENTS];
	int  swarm_size;
} tracker_file_t;

typedef struct {
	tracker_file_t files[MAX_FILES];
	int file_count;

	int clients_done;    // how many clients have sent MSG_FINISH_FILE
	int total_clients;   // = numtasks - 1
} tracker_t;

/* Send full file_t */
static void send_file(const file_t *f, int dest, int tag)
{
	MPI_Send(f->name, MAX_FILENAME+1, MPI_CHAR, dest, tag, MPI_COMM_WORLD);
	MPI_Send(&f->total_chunks, 1, MPI_INT, dest, tag, MPI_COMM_WORLD);
	for(int i=0; i< f->total_chunks; i++){
		MPI_Send(f->chunks[i], HASH_SIZE+1, MPI_CHAR, dest, tag, MPI_COMM_WORLD);
	}
}

/* Receive full file_t */
static void recv_file(file_t *f, int src, int tag)
{
	MPI_Status st;
	MPI_Recv(f->name, MAX_FILENAME+1, MPI_CHAR, src, tag, MPI_COMM_WORLD, &st);
	MPI_Recv(&f->total_chunks, 1, MPI_INT, src, tag, MPI_COMM_WORLD, &st);
	for(int i=0; i< f->total_chunks; i++){
		MPI_Recv(f->chunks[i], HASH_SIZE+1, MPI_CHAR, src, tag, MPI_COMM_WORLD, &st);
	}
}

/* ========================= Download Thread ========================= */

void save_file(client_t *c, wanted_file_t *wf) {
	char file_name[30];
	sprintf(file_name,"client%d_%s", c->rank, wf->name);

	FILE *f= fopen(file_name,"w");
	if (f) {
		for (int i = 0; i < wf->total_chunks; i++) {
			fprintf(f, "%s\n", wf->seg_hashes[i]);
		}
		fclose(f);
	}
}

void *download_thread_func(void *arg)
{
	client_t *c = (client_t*)arg;
	MPI_Status st;

	/* Go through all wanted files */
	for (int i = 0; i < c->num_wanted; i++){
		wanted_file_t *wf = &c->wanted[i];

		/* Send swarm request (hashes + seeds) */
		MPI_Send(wf->name, MAX_FILENAME+1, MPI_CHAR,
				 TRACKER_RANK, MSG_REQUEST_SWARM, MPI_COMM_WORLD);

		/* Receive number of total chuncks */
		MPI_Recv(&wf->total_chunks, 1, MPI_INT,
				 TRACKER_RANK, MSG_SWARM_INFO, MPI_COMM_WORLD, &st);

		/* Treat possible error */
		if (wf->total_chunks <= 0) {
			// file not found => skip
			continue;
		}

		/* Receive al segment hashes */
		for(int i = 0; i < wf->total_chunks; i++){
			MPI_Recv(wf->seg_hashes[i], HASH_SIZE + 1, MPI_CHAR,
					 TRACKER_RANK, MSG_SWARM_INFO, MPI_COMM_WORLD, &st);
			wf->have_chunk[i] = 0;
		}

		/* Receive number of clients in swarm list (swarm_size) and their names */
		int swarm_size;
		MPI_Recv(&swarm_size, 1, MPI_INT,
				 TRACKER_RANK, MSG_SWARM_INFO, MPI_COMM_WORLD, &st);
		int swarm_list[MAX_CLIENTS];
		for(int i = 0; i < swarm_size; i++){
			MPI_Recv(&swarm_list[i], 1, MPI_INT,
					 TRACKER_RANK, MSG_SWARM_INFO, MPI_COMM_WORLD, &st);
		}

		/* Download segments */
		int downloaded = 0;
		int downloaded_10 = 0;

		while (downloaded < wf->total_chunks) {
			for (int seg = 0; seg < wf->total_chunks; seg++) {
				/* If already downloaded */
				if (wf->have_chunk[seg]) {
					continue;
				}

				/* Try to download the `seg' segment from any seed in the swarm */
				for (int seed = 0; seed < swarm_size; seed++) {

					/* Try in a cyclic way */
					int srank = swarm_list[(seg + seed) % swarm_size];

					/* Send the request (filename + index_segment) */
                    MPI_Send(&seg, 1, MPI_INT,
							 srank, MSG_REQUEST_SEG, MPI_COMM_WORLD);
					MPI_Send(wf->name, MAX_FILENAME + 1, MPI_CHAR,
							 srank, MSG_REQUEST_SEG, MPI_COMM_WORLD);

					/* Get a response */
					char resp[4];
					MPI_Recv(resp, 4, MPI_CHAR,
							 srank, MSG_ACK_SEG, MPI_COMM_WORLD, &st);

					if (strcmp(resp, "OK") == 0) {
						/* Segment was successfully downloaded */
						wf->have_chunk[seg] = 1;
						downloaded++;
						downloaded_10++;
						break;
					}
				}

				/* After downloading 10 segments => only ask for new seeds */
				if (downloaded_10 == 10 && downloaded < wf->total_chunks) {
					downloaded_10 = 0;

					MPI_Send(wf->name, MAX_FILENAME + 1, MPI_CHAR,
							 TRACKER_RANK, MSG_REQUEST_SEEDS, MPI_COMM_WORLD);

					/* Receive the seeds list */
					MPI_Recv(&swarm_size, 1, MPI_INT,
							 TRACKER_RANK, MSG_SWARM_INFO, MPI_COMM_WORLD, &st);
					for (int s = 0; s < swarm_size; s++) {
						MPI_Recv(&swarm_list[s], 1, MPI_INT,
								 TRACKER_RANK, MSG_SWARM_INFO, MPI_COMM_WORLD, &st);
					}
				}
			}
		}

		/* Check if all segments have been downloaded */
		if(downloaded == wf->total_chunks){
			/* Save the file */
			save_file(c, wf);

			/* Announce the tracker */
			MPI_Send(wf->name, MAX_FILENAME+1, MPI_CHAR,
					 TRACKER_RANK, MSG_FINISH_FILE, MPI_COMM_WORLD);
		}
	}

	/* 3) Am terminat tot => MSG_FINISH_ALL */
	printf("Client %d: Completed ALL => MSG_FINISH_ALL\n", c->rank);
	MPI_Send(NULL,0,MPI_BYTE, TRACKER_RANK, MSG_FINISH_ALL, MPI_COMM_WORLD);

	pthread_exit(NULL);
}

/* ========================= Upload Thread ========================= */
void *upload_thread_func(void *arg)
{
	client_t *c= (client_t*)arg;
	MPI_Status st;

	while (1) {
		/* Check if MSG_TERMINATE tag received from tracker */
		int flag_terminate = 0;
		MPI_Iprobe(TRACKER_RANK, MSG_TERMINATE,
				   MPI_COMM_WORLD, &flag_terminate, &st);
		if(flag_terminate){
			/* Consume the message "DONE" */
			char message[5];
			MPI_Recv(message, 5, MPI_CHAR, TRACKER_RANK,
					 MSG_TERMINATE, MPI_COMM_WORLD, &st);
			break;
		}

		/* Check if MSG_REQUEST_SEG tag received from tracker */
		int flag_request = 0;
		MPI_Iprobe(MPI_ANY_SOURCE, MSG_REQUEST_SEG, MPI_COMM_WORLD, &flag_request, &st);
		if(!flag_request){
			continue;
		}

		/* Received MSG_REQUEST_SEG */
		char file_name[MAX_FILENAME + 1];
		int seg_idx;

		/* Receive the filename and the segment index wanted */
		MPI_Recv(&seg_idx, 1, MPI_INT, st.MPI_SOURCE, MSG_REQUEST_SEG, MPI_COMM_WORLD, &st);
        MPI_Recv(file_name, MAX_FILENAME + 1, MPI_CHAR, st.MPI_SOURCE, MSG_REQUEST_SEG, MPI_COMM_WORLD, &st);

		/* Check if file exists */
		int idx= -1;
		for (int i = 0; i < c->num_owned; i++) {
			if (strcmp(file_name, c->owned[i].name) == 0) {
				idx = i;
				break;
			}
		}

		/* Treat a possible error if the file is not found or if client does not have the required segment */
		if (idx < 0 || seg_idx < 0 || seg_idx >= c->owned[idx].total_chunks) {
			MPI_Send("NA", 3, MPI_CHAR, st.MPI_SOURCE, MSG_ACK_SEG, MPI_COMM_WORLD);
		} else {
			/* Send ACK -> OK */
			MPI_Send("OK", 3, MPI_CHAR, st.MPI_SOURCE, MSG_ACK_SEG, MPI_COMM_WORLD);
		}
	}
	pthread_exit(NULL);
}

/* ========================= tracker ========================= */
static void tracker_recv_files(tracker_t *td, int rank)
{
	MPI_Status st;
	int nfiles;
	/* First, receive the number of files */
	MPI_Recv(&nfiles, 1, MPI_INT, rank, MSG_INIT, MPI_COMM_WORLD, &st);

	/* Then, get all files and create their swarm */
	for (int i = 0; i < nfiles; i++){
		file_t f;
		recv_file(&f, rank, MSG_INIT);

		/* Check if the file has been already downloaded*/
		int idx = -1;
		for (int j = 0; j < td->file_count; j++){
			if (strcmp(f.name, td->files[j].file_name) == 0) {
				idx = j;
				break;
			}
		}

		/* If file doesn't exist, add it */
		if (idx < 0){
			idx = td->file_count++;
			strcpy(td->files[idx].file_name, f.name);
			td->files[idx].total_chunks = f.total_chunks;
			td->files[idx].swarm_size = 0;
			for(int j = 0; j < f.total_chunks; j++){
				strcpy(td->files[idx].chunks[j], f.chunks[j]);
			}
		}

		/* Add client's rank in file's swarm list */
		td->files[idx].swarm[ td->files[idx].swarm_size++ ]= rank;
	}
}

void tracker_handle_request_swarm(char *fname, int src, tracker_t *tdata, MPI_Status st)
{
	/* Receive request swarm message */
	MPI_Recv(fname, MAX_FILENAME+1, MPI_CHAR, src, MSG_REQUEST_SWARM, MPI_COMM_WORLD, &st);

	/* Find the index of the file */
	int idx = -1;
	for (int i = 0; i < tdata->file_count; i++){
		if (strcmp(fname, tdata->files[i].file_name) == 0){
			idx = i;
			break;
		}
	}

	/* Treat a possible error if the file is not found (this should not happen)*/
	if(idx < 0){
		int zero = 0;
		MPI_Send(&zero,1,MPI_INT, src, MSG_SWARM_INFO, MPI_COMM_WORLD);
		return;
	}

	/* Send the number of total chunks of the file */
	int total_chunks = tdata->files[idx].total_chunks;
	MPI_Send(&total_chunks, 1, MPI_INT, src, MSG_SWARM_INFO, MPI_COMM_WORLD);

	/* Send all segment hashes */
	for(int i = 0; i < total_chunks; i++){
		MPI_Send(tdata->files[idx].chunks[i], HASH_SIZE + 1, MPI_CHAR, src, MSG_SWARM_INFO, MPI_COMM_WORLD);
	}

	/* Send the number of clients in file's swarm */
	MPI_Send(&tdata->files[idx].swarm_size, 1, MPI_INT, src, MSG_SWARM_INFO, MPI_COMM_WORLD);

	/* Send the names of all clients that own this file */
	for(int i = 0; i < tdata->files[idx].swarm_size; i++){
		MPI_Send(&tdata->files[idx].swarm[i], 1, MPI_INT, src, MSG_SWARM_INFO, MPI_COMM_WORLD);
	}
}

void tracker_handle_request_seeds(char *fname, int src, tracker_t *tdata, MPI_Status st)
{
	MPI_Recv(fname, MAX_FILENAME+1, MPI_CHAR, src, MSG_REQUEST_SEEDS, MPI_COMM_WORLD, &st);

	/* Find the index of the file */
	int idx = -1;
	for (int i = 0; i < tdata->file_count; i++){
		if (strcmp(fname, tdata->files[i].file_name) == 0){
			idx = i;
			break;
		}
	}

	/* Treat a possible error if the file is not found (this should not happen)*/
	if(idx < 0){
		int zero = 0;
		MPI_Send(&zero,1,MPI_INT, src, MSG_SWARM_INFO, MPI_COMM_WORLD);
		return;
	}

	/* Send the number of clients in file's swarm */
	MPI_Send(&tdata->files[idx].swarm_size, 1, MPI_INT, src, MSG_SWARM_INFO, MPI_COMM_WORLD);

	/* Send the names of all clients that own this file */
	for(int i = 0; i < tdata->files[idx].swarm_size; i++){
		MPI_Send(&tdata->files[idx].swarm[i], 1, MPI_INT, src, MSG_SWARM_INFO, MPI_COMM_WORLD);
	}

}
void tracker_handle_finish_file(char *fname, int src, tracker_t *tdata, MPI_Status st)
{
	/* A client has finished a file download */
	MPI_Recv(fname, MAX_FILENAME + 1, MPI_CHAR, src, MSG_FINISH_FILE, MPI_COMM_WORLD, &st);

	/* Find the index of the file*/
	int idx = -1;
	for (int i = 0; i < tdata->file_count; i++) {
		if (strcmp(fname, tdata->files[i].file_name) == 0) {
			idx = i;
			break;
		}
	}

	/* Treat a possible error if the file is not found (this should not happen)*/
	if(idx < 0){
		return;
	}

	/* Check if client already exists in swarm list */
	int exists = 0;
	for (int i = 0; i < tdata->files[idx].swarm_size; i++){
		if (tdata->files[idx].swarm[i] == src) {
			exists = 1;
			break;
		}
	}

	/* Add client as seed in swarm list */
	if (!exists){
		tdata->files[idx].swarm[ tdata->files[idx].swarm_size++ ]= src;
	}
}

int tracker_handle_finish_all(int numtasks, int src, tracker_t *tdata, MPI_Status st)
{
	/* A client has finished all its files download */
	MPI_Recv(NULL, 0, MPI_CHAR, src, MSG_FINISH_ALL, MPI_COMM_WORLD, &st);

	/* Increase the number of clients that has finished downloading all their files */
	tdata->clients_done++;

	/* If all clients are done -> tracker sends MSG_TERMINATE tag to all clients */
	if(tdata->clients_done == tdata->total_clients){
		for(int i = 1; i < numtasks; i++){
			MPI_Send("DONE", 4, MPI_CHAR, i, MSG_TERMINATE, MPI_COMM_WORLD);
		}
		return 1;
	}
	return 0;
}

void tracker(int numtasks,int rank)
{
	tracker_t tdata;
	tdata.file_count = 0;
	tdata.clients_done = 0;
	tdata.total_clients = numtasks-1;

	/* Receive files from all clients */
	for(int i = 1; i < numtasks; i++) {
		tracker_recv_files(&tdata, i);
		/* Send initial ack to inform all clients that the download process can start */
		MPI_Send("OK", 3, MPI_CHAR, i, MSG_INIT_ACK, MPI_COMM_WORLD);
	}

	char fname[MAX_FILENAME+1];

	int done = 0;
	while(!done) {

		/* Receive any tag */
		MPI_Status st;
		MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &st);
		int src= st.MPI_SOURCE, tag= st.MPI_TAG;

		switch (tag) {
			case MSG_REQUEST_SWARM:
				tracker_handle_request_swarm(fname, src, &tdata, st);
				break;
			case MSG_REQUEST_SEEDS:
				tracker_handle_request_seeds(fname, src, &tdata, st);
				break;
			case MSG_FINISH_FILE:
				tracker_handle_finish_file(fname, src, &tdata, st);
				break;
			case MSG_FINISH_ALL:
				done = tracker_handle_finish_all(numtasks, src, &tdata, st);
				break;
			default:
				/* Ignore any other message */
				MPI_Recv(NULL,0,MPI_CHAR, src, tag, MPI_COMM_WORLD, &st);
				break;
		}
	}
}

/* ============== read input file ============== */
static void read_input_file(client_t *c)
{
	char fname[MAX_FILENAME];
	sprintf(fname,"in%d.txt", c->rank);

	FILE *fp= fopen(fname,"r");
	if(!fp){
		fprintf(stderr,"Cannot open %s\n", fname);
		MPI_Abort(MPI_COMM_WORLD,1);
	}

	/* Read the number of files owned by the client */
	if (fscanf(fp,"%d",&c->num_owned) != 1) {
		fprintf(stderr, "Error: Failed to read num owned.\n");
		fclose(fp);
		exit(EXIT_FAILURE);
	}

	for(int i=0; i< c->num_owned; i++){

		/* Read the filename and its total number of chunks */
		if (fscanf(fp, "%s %d", c->owned[i].name, &c->owned[i].total_chunks) != 2) {
			fprintf(stderr, "Error: Failed to read file name or chunk count.\n");
			fclose(fp);
			exit(EXIT_FAILURE);
		}

		for(int s=0; s< c->owned[i].total_chunks; s++){

			/* Read each hash */
			if (fscanf(fp,"%s", c->owned[i].chunks[s]) != 1) {
				fprintf(stderr, "Error: Failed to read chunk hash.\n");
				fclose(fp);
				exit(EXIT_FAILURE);
			}
		}
	}

	/* Read the number of files wanted */
	if (fscanf(fp,"%d",&c->num_wanted) != 1) {
		fprintf(stderr, "Error: Failed to read num wanted.\n");
			fclose(fp);
			exit(EXIT_FAILURE);
	}

	for(int i=0; i< c->num_wanted; i++){
		wanted_file_t *wf= &c->wanted[i];

		/* Read each filename */
		if (fscanf(fp,"%s",wf->name) != 1) {
			fprintf(stderr, "Error: Failed to read file name.\n");
			fclose(fp);
			exit(EXIT_FAILURE);
		}
	}

	fclose(fp);
}

/* ============== init_client ============== */
static client_t* init_client(int rank)
{
	client_t *c= (client_t*)calloc(1,sizeof(client_t));
	c->rank= rank;
	c->finished_all=0;

	read_input_file(c);

	/* Send to tracker: the number of files owned and the files */
	MPI_Send(&c->num_owned,1,MPI_INT, TRACKER_RANK, MSG_INIT, MPI_COMM_WORLD);
	for(int i=0; i< c->num_owned; i++){
		send_file(&c->owned[i], TRACKER_RANK, MSG_INIT);
	}

	/* Wait for ACK from tracker */
	char ack[3];
	MPI_Status st;
	MPI_Recv(ack,3,MPI_CHAR, TRACKER_RANK, MSG_INIT_ACK, MPI_COMM_WORLD, &st);

	return c;
}

/* ============== peer ============== */
void peer(int numtasks, int rank) {
	pthread_t download_thread;
	pthread_t upload_thread;
	void *status;
	int r;

	client_t *c = init_client(rank);

	r = pthread_create(&download_thread, NULL, download_thread_func, (void *)c);
	if (r) {
		printf("Eroare la crearea thread-ului de download\n");
		exit(-1);
	}

	r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *)c);
	if (r) {
		printf("Eroare la crearea thread-ului de upload\n");
		exit(-1);
	}

	r = pthread_join(download_thread, &status);
	if (r) {
		printf("Eroare la asteptarea thread-ului de download\n");
		exit(-1);
	}

	r = pthread_join(upload_thread, &status);
	if (r) {
		printf("Eroare la asteptarea thread-ului de upload\n");
		exit(-1);
	}

	free(c);
}

int main (int argc, char *argv[]) {
	int numtasks, rank;

	int provided;
	MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
	if (provided < MPI_THREAD_MULTIPLE) {
		fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
		exit(-1);
	}
	MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);

	if (rank == TRACKER_RANK) {
		tracker(numtasks, rank);
	} else {
		peer(numtasks, rank);
	}

	MPI_Finalize();
}
