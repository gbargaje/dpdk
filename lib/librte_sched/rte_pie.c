#define DROP 0
#define ENQUE 1
#define QDELAY_REF 	15	//in milliseconds
#define MAX_BURST 	150	//in milliseconds

#define T_UPDATE	15	//in milliseconds
#define MEAN_PKTSIZE	//to be decided

double alpha = 0.125;
double beta = 1.25;

#include <stdio.h>
#include <rte_pie.h>
#include <rte_random.h>
#include <rte_timer.h>

struct rte_pie_status {
	double burst_allowance;	//burst allowance, initialized with value of MAX_BURST.
	double drop_prob;	//Drop rate
	double qdelay_old;	//Old queue delay
	double qdelay;		//current queue delay
	//double accu_prob;
};

/*
struct rte_pie_params
{
	double alpha;
	double beta;
	spinlock --- to be implemented..
};
*/

void init_pie(struct rte_pie_status *PIE) {
	PIE -> burst_allowance = MAX_BURST;
	PIE -> drop_prob = 0;
	PIE -> qdelay_old = 0;
	PIE -> qdelay = 0;
	//PIE -> accu_prob = 0;
}

/*
 * To ensure that PIE is "work conserving", we bypass the random drop if
 * the latency sample, PIE->qdelay_old_, is smaller than half of the
 * target latency value (QDELAY_REF) when the drop probability is not
 * too high (i.e., PIE->drop_prob_ < 0.2), or if the queue has less than
 * a couple of packets.
 */

static uint64_t get_current_queue_delay(struct rte_pie_status *PIE){
	return PIE -> qdelay;
}

//add synchronization
static void set_current_qdelay(struct rte_pie_status *PIE, uint64_t timestamp){
	uint64_t now = rte_get_tsc_cycles();		//get the total number of cycles since boot.
	uint64_t cycles_in_second = rte_get_timer_hz(); //get the number of cycles in one second
	now /= cycles_in_second / 1000;	// get the timestamp in milliseconds.
	PIE->qdelay = now - timestamp;	//current queue delay in milliseconds.
}

static int drop_early() {
	//Safeguard PIE to be work conserving
	if((PIE->qdelay_old < QDELAY_REF/2 && PIE->drop_prob < 0.2) || (qlen <= 2 * MEAN_PKTSIZE))
		return ENQUE;

	//function from rte_random.h return random number less than argument specified.
	uint64_t u = rte_rand_max(1 << 22);
	if((double) u < PIE -> drop_prob)
		return DROP;
	return ENQUE;
}

//Called on each packet arrival
static int enque(struct rte_pie_status *PIE) {
	int current_qdelay = PIE -> qdelay;

	//burst allowance is multiple of t_update
	if (PIE->burst_allowance == 0 && drop_early() == DROP)
		return DROP;

	if (PIE->drop_prob == 0 && current_qdelay < QDELAY_REF/2 && PIE->qdelay_old < QDELAY_REF/2) {
			PIE->burst_allowance = MAX_BURST;
	}
	return ENQUE;
}

//Update periodically, T_UPDATE = 15 milliseconds
static void calculate_drop_prob(struct rte_pie_status *PIE) {

	int current_qdelay = PIE-> qdelay;
	double p = alpha * (current_qdelay - QDELAY_REF) + beta * (current_qdelay - PIE->qdelay_old);

	//How alpha and beta are set?
	if (PIE->drop_prob < 0.000001) {
		p /= 2048;
	} else if (PIE->drop_prob < 0.00001) {
		p /= 512;
	} else if (PIE->drop_prob < 0.0001) {
		p /= 128;
	} else if (PIE->drop_prob < 0.001) {
		p /= 32;
	} else if (PIE->drop_prob < 0.01) {
		p /= 8;
	} else if (PIE->drop_prob < 0.1) {
		p /= 2;
	} else {
		p = p;
	}

	//Cap Drop Adjustment
	if (PIE->drop_prob >= 0.1 && p > 0.02) {
		p = 0.02;
	}
	PIE->drop_prob += p;

	//Exponentially decay drop prob when congestion goes away
	if (current_qdelay < QDELAY_REF/2 && PIE->qdelay_old < QDELAY_REF/2) {
		PIE->drop_prob *= 0.98;        //1 - 1/64 is sufficient
	}

	//Bound drop probability
	if (PIE->drop_prob < 0)
		PIE->drop_prob = 0;
	if (PIE->drop_prob > 1)
		PIE->drop_prob = 1;

	//Burst tolerance
	PIE->burst_allowance = max(0, PIE->burst_allowance - T_UPDATE);
	PIE->qdelay_old = current_qdelay;
}

//Called on each packet departure
static void deque(struct rte_pie_status *PIE, uint64 timestamp) {
	set_current_qdelay(PIE, timestamp);
}
