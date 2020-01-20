#ifndef __RTE_PIE_H_INCLUDED__
#define __RTE_PIE_H_INCLUDED__

#ifdef __cplusplus
extern "C" {
#endif

#define DROP 			0
#define ENQUE 			1
#define QDELAY_REF 		15		//in milliseconds
#define MAX_BURST 		150		//in milliseconds

#define T_UPDATE		15		//in milliseconds
#define MEAN_PKTSIZE	1500	//to be decided

#include <stdio.h>
#include <rte_common.h>
#include <rte_random.h>
#include <rte_timer.h>
#define DQ_THRESHOLD 1 << 14	//16KB

struct rte_pie_config{
	double burst_allowance;	//burst allowance, initialized with value of MAX_BURST.
	double drop_prob;	//Drop rate
	double qdelay_old;	//Old queue delay
	double qdelay;		//current queue delay
	double alpha;
	double beta;
	//double accu_prob;
};

/*
struct rte_pie_params {
	double burst_allowance;	//burst allowance, initialized with value of MAX_BURST.
	double drop_prob;	//Drop rate
	double qdelay_old;	//Old queue delay
	double qdelay;		//current queue delay
};

struct rte_pie {
	double drop_prob;
	double alpha;
	double beta;
};
*/

void rte_pie_config_init(struct rte_pie_config *);
int rte_pie_drop(struct rte_pie_config*, uint64_t);
int rte_pie_enque(struct rte_pie_config *, uint64_t);
//static void rte_pie_deque(struct rte_pie_config *, uint64_t);
//void rte_pie_set_current_qdelay(struct rte_pie_config *, uint64_t)
//static void rte_pie_calculate_drop_prob(struct rte_pie_config *);

static inline uint64_t max(uint64_t a, uint64_t b){
	if(a > b)
		return a;
	return b;
}

static inline uint64_t rte_pie_get_current_queue_delay(struct rte_pie_config *pie_config){
	return pie_config->qdelay;
}

//Update periodically, T_UPDATE = 15 milliseconds
__rte_unused static void rte_pie_calculate_drop_prob(
		__attribute__((unused)) struct rte_timer *tim,	void *arg) {
	struct rte_pie_config *pie_config = (struct rte_pie_config *) arg;
	int current_qdelay = pie_config -> qdelay;
	double p = pie_config->alpha * (current_qdelay - QDELAY_REF) + \
			pie_config->beta * (current_qdelay - pie_config ->qdelay_old);

	//How alpha and beta are set?
	if (pie_config->drop_prob < 0.000001) {
		p /= 2048;
	} else if (pie_config->drop_prob < 0.00001) {
		p /= 512;
	} else if (pie_config->drop_prob < 0.0001) {
		p /= 128;
	} else if (pie_config->drop_prob < 0.001) {
		p /= 32;
	} else if (pie_config->drop_prob < 0.01) {
		p /= 8;
	} else if (pie_config->drop_prob < 0.1) {
		p /= 2;
	} else {
		p = p;
	}

	//Cap Drop Adjustment
	if (pie_config->drop_prob >= 0.1 && p > 0.02) {
		p = 0.02;
	}
	pie_config->drop_prob += p;

	//Exponentially decay drop prob when congestion goes away
	if (current_qdelay < QDELAY_REF/2 && pie_config->qdelay_old < QDELAY_REF/2) {
		pie_config->drop_prob *= 0.98;        //1 - 1/64 is sufficient
	}

	//Bound drop probability
	if (pie_config->drop_prob < 0)
		pie_config->drop_prob = 0;
	if (pie_config->drop_prob > 1)
		pie_config->drop_prob = 1;

	//Burst tolerance
	pie_config->burst_allowance = max(0, pie_config->burst_allowance - T_UPDATE);
	pie_config->qdelay_old = current_qdelay;
}


//Called on each packet departure
static inline void rte_pie_deque(struct rte_pie_config *pie_config, uint64_t timestamp) {
	//rte_pie_set_current_qdelay(pie_config, timestamp);
	uint64_t now = rte_get_tsc_cycles();		//get the total number of cycles since boot.
	//uint64_t cycles_in_second = rte_get_timer_hz(); //get the number of cycles in one second
	//now /= cycles_in_second / 1000;	// get the timestamp in milliseconds.
	pie_config->qdelay = now - timestamp;	//current queue delay in milliseconds.
}


#ifdef __cplusplus
}
#endif

#endif /* __RTE_PIE_H_INCLUDED__ */

