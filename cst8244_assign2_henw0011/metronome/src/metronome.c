struct ioattr_t;
#define IOFUNC_ATTR_T struct ioattr_t
struct metronome_ocb_s;
#define IOFUNC_OCB_T struct metronome_ocb_s

#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <math.h>
#include <sys/types.h>
#include <sys/netmgr.h>
#include <sys/neutrino.h>

//PULSE CODES
#define METRONOME_PULSE	_PULSE_CODE_MINAVAIL
#define PAUSE_PULSE		(_PULSE_CODE_MINAVAIL + 1)
#define SET_PULSE		(_PULSE_CODE_MINAVAIL + 2)
#define START_PULSE		(_PULSE_CODE_MINAVAIL + 3)
#define STOP_PULSE		(_PULSE_CODE_MINAVAIL + 4)
#define QUIT_PULSE		(_PULSE_CODE_MINAVAIL + 5)

//PATH
#define METRO_ATTACH  "metronome"

//DEVICES
#define DEVICES 2
#define METRONOME 0
#define METRONOME_HELP 1

//Contains paths for all devices
char *deviceNames[DEVICES] = { "/dev/local/metronome",
		"/dev/local/metronome-help" };

//METRONOME STATUS
#define START 0
#define STOPPED 1
#define PAUSED 2

//Pulse message
typedef union {
	struct _pulse pulse;
	char msg[255];
} my_message_t;

//DATA TABLES (OUTPUTS)
struct DataTableRow {
	int timeSigTop;
	int timeSigBot;
	int intervals;
	char pattern[16];
};

struct DataTableRow t[] = { { 2, 4, 4, "|1&2&" }, { 3, 4, 6, "|1&2&3&" }, { 4,
		4, 8, "|1&2&3&4&" }, { 5, 4, 10, "|1&2&3&4-5-" },
		{ 3, 8, 6, "|1-2-3-" }, { 6, 8, 6, "|1&a2&a" },
		{ 9, 8, 9, "|1&a2&a3&a" }, { 12, 8, 12, "|1&a2&a3&a4&a" } };

struct metronome_Timer {
	double bps;
	double measure;
	double interval;
	double nano_seconds;
}typedef metronome_timer_t;

struct metronome {
	int bpm;
	int tst;
	int tsb;
	metronome_timer_t m_timer;
}typedef metronome_t;

//OVERRIDE iofunc_attr_t
typedef struct ioattr_t {
	iofunc_attr_t attr;
	int device;
} ioattr_t;

typedef struct metronome_ocb_s {
	iofunc_ocb_t ocb;
	char* buffer; //user defined buffer
	int bufsize; //user defined variable
} metronome_ocb_t;

//GLOBAL VARIABLES
metronome_t metronome;
name_attach_t* attach;
int metronome_coid;
char data[255];

int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb); //Override POSIX function
int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb); //Override POSIX function
int io_open(resmgr_context_t *ctp, io_open_t *msg, RESMGR_HANDLE_T *handle,
		void *extra); //Override POSIX function
void set_timer(metronome_t * metronome); //Config timer settings
void start_timer(struct itimerspec * itime, timer_t timer_id,
		metronome_t* metronome); //Starts timer
metronome_ocb_t * metronome_ocb_calloc(resmgr_context_t *ctp,
		IOFUNC_ATTR_T *mtattr); //Allocate memory for ocb
void metronome_ocb_free(IOFUNC_OCB_T *mocb); //Free allocated memory for ocb

//Thread created in main runs this function
//Purpose: "Drive" the metronome
//Receives pulse from interval timer; each time the timer expires
//Receives pulses from io_write (quit and pause <int>)
void *metronome_thread() { //remove args later
	struct sigevent event; //Event that listens for pulse
	struct itimerspec itime;
	timer_t timer_id;
	my_message_t msg;
	int rcvid;
	int index = 0;
	char *pt;
	int timer_status = 0;

	//Phase I: Create a named channel to receive pulses
	if ((attach = name_attach(NULL, METRO_ATTACH, 0)) == NULL) {
		printf("Error: name_attach - metronome.c");
		exit(EXIT_FAILURE);
	}

	//Set up the event handler
	event.sigev_notify = SIGEV_PULSE;
	event.sigev_coid = ConnectAttach(ND_LOCAL_NODE, 0, attach->chid,
			_NTO_SIDE_CHANNEL, 0);
	event.sigev_priority = SIGEV_PULSE_PRIO_INHERIT;
	event.sigev_code = METRONOME_PULSE;

	//Create timer
	timer_create(CLOCK_REALTIME, &event, &timer_id);

	for (index = 0; index < 8; index++) { /* search through table */
		if (t[index].timeSigBot == metronome.tsb
				&& t[index].timeSigTop == metronome.tst) {
			break;
		}
	}

	set_timer(&metronome);

	start_timer(&itime, timer_id, &metronome);

	pt = t[index].pattern;

	//Phase II
	for (;;) {
		if ((rcvid = MsgReceive(attach->chid, &msg, sizeof(msg), NULL)) == -1) {
			printf("Error - MessageReceive() - ./metronome\n");
			exit(EXIT_FAILURE);
		}

		if (rcvid == 0) { //Received a pulse
			switch (msg.pulse.code) {
			case METRONOME_PULSE:
				if (*pt == '|') { //Beginning of bar
					printf("%.2s", pt);
					pt = (pt + 2);
				} else if (*pt == '\0') { //End of bar
					printf("\n");
					pt = t[index].pattern;
				} else { //Print next char from pattern
					printf("%c", *pt++);
				}
				break;

			case PAUSE_PULSE: //Pause timer, don't use sleep()
				if (timer_status == START) {
					itime.it_value.tv_sec = msg.pulse.value.sival_int; //Set the amount of time to pause
					timer_settime(timer_id, 0, &itime, NULL); //Pause the timer
				}
				break;

			case SET_PULSE: //Set a new pattern for the metronome
				//Get the index of the desired pattern
				for (index = 0; index < 8; index++) {
					if (t[index].timeSigBot == metronome.tsb
							&& t[index].timeSigTop == metronome.tst) {
						break;
					}
				}

				pt = t[index].pattern; //Point to the new pattern
				set_timer(&metronome); //Set timer
				start_timer(&itime, timer_id, &metronome); //Start timer
				printf("\n");
				break;

			case START_PULSE: //Start metronome if it is currently paused
				if (timer_status == STOPPED) { //Check if stopped
					start_timer(&itime, timer_id, &metronome);
					timer_status = START;
				}
				break;

			case STOP_PULSE: //Stop the metronome in its tracks
				if (timer_status == 0 || timer_status == 2) { //Check if its running or paused
				//stop_timer(&itime, timer_id);
					itime.it_value.tv_sec = 0;
					timer_settime(timer_id, 0, &itime, NULL);
					timer_status = STOPPED;
				}
				break;

			case QUIT_PULSE: //Immediately exits program
				timer_delete(timer_id); //Delete timer
				name_detach(attach, 0); //Detach name-space
				name_close(metronome_coid); //Close name-space
				exit(EXIT_SUCCESS);
			}

		}

		fflush(stdout);
	}
	return NULL;
}

void set_timer(metronome_t * metronome) {
	metronome->m_timer.bps = (double) 60 / metronome->bpm; //SPB
	metronome->m_timer.measure = metronome->m_timer.bps * 2; //BPM
	metronome->m_timer.interval = metronome->m_timer.measure / metronome->tsb; //SPI
	metronome->m_timer.nano_seconds = (metronome->m_timer.interval
			- (int) metronome->m_timer.interval) * 1e+9; //Nano Seconds
}

void start_timer(struct itimerspec * itime, timer_t timer_id,
		metronome_t* metronome) {
	itime->it_value.tv_sec = 1;
	itime->it_value.tv_nsec = 0;
	itime->it_interval.tv_sec = metronome->m_timer.interval;
	itime->it_interval.tv_nsec = metronome->m_timer.nano_seconds;
	timer_settime(timer_id, 0, itime, NULL);
}

int io_read(resmgr_context_t *ctp, io_read_t *msg, metronome_ocb_t *mocb) {
	int nb;
	int index = 0;

	if (data == NULL)
		return 0;

	//metronome-help
	if (mocb->ocb.attr->device == 1) { /* Check if device is /dev/local/metronome-help */
		sprintf(data,
				"Metronome Resource Manager (ResMgr)\n\nUsage: metronome <bpm> <ts-top> <ts-bottom>\n\nAPI:\n pause[1-9]\t\t\t- pause the metronome for 1-9 seconds\n quit:\t\t\t\t- quit the metronome\n set <bpm> <ts-top> <ts-bottom>\t- set the metronome to <bpm> ts-top/ts-bottom\n start\t\t\t\t- start the metronome from stopped state\n stop\t\t\t\t- stop the metronome; use 'start' to resume\n");
		//metronome
	} else {

		//Search for signatures in the data table
		for (index = 0; index < 8; index++) { /* search through table */
			if (t[index].timeSigBot == metronome.tsb
					&& t[index].timeSigTop == metronome.tst) {
				break;
			}
		}

		sprintf(data,
				"[metronome: %d beats/min, time signature: %d/%d, sec-per-interval: %.2f, nanoSecs: %.0lf]\n",
				metronome.bpm, t[index].timeSigTop, t[index].timeSigBot,
				metronome.m_timer.interval, metronome.m_timer.nano_seconds);
	}

	nb = strlen(data);

	//Test to see if we have already sent the whole message.
	if (mocb->ocb.offset == nb)
		return 0;

	//We will return which ever is smaller the size of our data or the size of the buffer
	nb = min(nb, msg->i.nbytes);

	//Set the number of bytes we will return
	_IO_SET_READ_NBYTES(ctp, nb);

	//Copy data into reply buffer.
	SETIOV(ctp->iov, data, nb);

	//update offset into our data used to determine start position for next read.
	mocb->ocb.offset += nb;

	//If we are going to send any bytes update the access time for this resource.
	if (nb > 0)
		mocb->ocb.flags |= IOFUNC_ATTR_ATIME;

	return (_RESMGR_NPARTS(1));
}

int io_write(resmgr_context_t *ctp, io_write_t *msg, metronome_ocb_t *mocb) {
	int nb = 0;

	//metronome-help
	if (mocb->ocb.attr->device == 1) {
		printf(
				"Error: Cannot write to device at path /dev/local/metronome-help\n");
		nb = msg->i.nbytes;
		_IO_SET_WRITE_NBYTES(ctp, nb);
		return (_RESMGR_NPARTS(0));
	}

	if (msg->i.nbytes == ctp->info.msglen - (ctp->offset + sizeof(*msg))) {
		//Have all the data
		char *buf;
		char *pause_msg;
		char *set_msg;
		int i, small_integer = 0;
		buf = (char *) (msg + 1);

		//Pause, Set, Start, Stop, Quit
		if (strstr(buf, "pause") != NULL) {
			for (i = 0; i < 2; i++) {
				pause_msg = strsep(&buf, " ");
			}
			small_integer = atoi(pause_msg);
			if (small_integer >= 1 && small_integer <= 9) {
				MsgSendPulse(metronome_coid, SchedGet(0, 0, NULL), PAUSE_PULSE,
						small_integer);
			} else {
				printf("pause <int> - <int> must be in range: 1 - 9\n");
			}

		} else if (strstr(buf, "set") != NULL) {
			//Split up the 4 command segments, you only need 3
			for (i = 0; i < 4; i++) {
				set_msg = strsep(&buf, " ");

				switch (i) {
				case 1:
					metronome.bpm = atoi(set_msg);
					break;
				case 2:
					metronome.tst = atoi(set_msg);
					break;
				case 3:
					metronome.tsb = atoi(set_msg);
					break;
				}
			}

			MsgSendPulse(metronome_coid, SchedGet(0, 0, NULL), SET_PULSE,
					small_integer);

		} else if (strstr(buf, "start") != NULL) {
			MsgSendPulse(metronome_coid, SchedGet(0, 0, NULL), START_PULSE,
					small_integer);
		} else if (strstr(buf, "stop") != NULL) {
			MsgSendPulse(metronome_coid, SchedGet(0, 0, NULL), STOP_PULSE,
					small_integer);
		} else if (strstr(buf, "quit") != NULL) {
			MsgSendPulse(metronome_coid, SchedGet(0, 0, NULL), QUIT_PULSE,
					small_integer);
		} else {
			set_msg = strsep(&buf, " ");
			printf("Error - '%s' is not a valid command\n", set_msg);
			//strcpy(data, buf);
			printf("Here");
		}

		nb = msg->i.nbytes;
	}

	_IO_SET_WRITE_NBYTES(ctp, nb);

	if (msg->i.nbytes > 0) {
		mocb->ocb.flags |= IOFUNC_ATTR_MTIME | IOFUNC_ATTR_CTIME;
	}

	return (_RESMGR_NPARTS(0));
}

int io_open(resmgr_context_t *ctp, io_open_t *msg, RESMGR_HANDLE_T *handle,
		void *extra) {
	if ((metronome_coid = name_open(METRO_ATTACH, 0)) == -1) {
		perror("name_open failed\n");
		return EXIT_FAILURE;
	}
	return (iofunc_open_default(ctp, msg, &handle->attr, extra));
}

metronome_ocb_t *metronome_ocb_calloc(resmgr_context_t *ctp, ioattr_t *tattr) {
	metronome_ocb_t *mocb;
	mocb = calloc(1, sizeof(metronome_ocb_t));
	mocb->ocb.offset = 0;
	return (mocb);
}

void metronome_ocb_free(metronome_ocb_t *mocb) {
	//free any other user defined memory here
	free(mocb);
}

int main(int argc, char* argv[]) {
	dispatch_t* dpp;
	resmgr_io_funcs_t io_funcs;
	resmgr_connect_funcs_t connect_funcs;
	ioattr_t io_attr[DEVICES];
	int id = 0;
	pthread_attr_t thread_attrib;
	dispatch_context_t *ctp;

	//Verify that there are 4 command-line arguments
	if (argc != 4) {
		printf(
				"Usage: ./metronome <beats-per-minute> <time-signature-top> <time-signature-bottom>\n");
		return EXIT_FAILURE;
	}

	//Process the command-line arguments
	metronome.bpm = atoi(argv[1]);
	metronome.tst = atoi(argv[2]);
	metronome.tsb = atoi(argv[3]);

	//Defines which functions to call during ocb memory resizing
	iofunc_funcs_t metronome_ocb_funcs = {
	_IOFUNC_NFUNCS, metronome_ocb_calloc, metronome_ocb_free, };

	iofunc_mount_t metronome_mount = { 0, 0, 0, 0, &metronome_ocb_funcs };

	//Create the dispatch structure
	if ((dpp = dispatch_create()) == NULL) {
		fprintf(stderr, "%s:  Unable to allocate dispatch context.\n", argv[0]);
		return (EXIT_FAILURE);
	}

	//Generate default resource manager i/o functions. Default actions will be taken for POSIX messages not handled in this program
	iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS,
			&io_funcs);
	//Overload the following functions
	connect_funcs.open = io_open;
	io_funcs.read = io_read;
	io_funcs.write = io_write;

	//Register more than one path name for the same resource manager (call resmgr_attach() more than once)
	for (int i = 0; i < DEVICES; i++) {
		//Initialize the resource manager's attribute struct
		//Using the timer example given in Writing Resource Managers - Part 3
		iofunc_attr_init(&io_attr[i].attr, S_IFCHR | 0666, NULL, NULL);
		io_attr[i].device = i;
		io_attr[i].attr.mount = &metronome_mount;

		//Attach device path to the resource manager
		if ((id = resmgr_attach(dpp, NULL, deviceNames[i], _FTYPE_ANY, 0,
				&connect_funcs, &io_funcs, &io_attr[i])) == -1) {
			fprintf(stderr, "%s:  Unable to attach name.\n", argv[0]);
			return (EXIT_FAILURE);
		}
	}

	ctp = dispatch_context_alloc(dpp);

	//Initiate and create thread
	pthread_attr_init(&thread_attrib);
	pthread_create(NULL, &thread_attrib, &metronome_thread, &metronome);

	while (1) {
		ctp = dispatch_block(ctp);
		dispatch_handler(ctp);
	}

	pthread_attr_destroy(&thread_attrib); //Destroy thread
	name_detach(attach, 0); //Detach name-space
	name_close(metronome_coid); //Close server
	return EXIT_SUCCESS;
}
