#define NROPES 16
#define NTHREADS 5
#define NSLEEP 0

/*
* Data structure to the connecting rope between a hook and a stake
*/
struct rope {
	struct lock *rp_lock;
	int rp_hook;
	volatile int rp_stake;
	volatile bool rp_cut;
};
