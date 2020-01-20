#define DROP 			0
#define ENQUE 			1
#define QDELAY_REF 		15		//in milliseconds
#define MAX_BURST 		150		//in milliseconds

#define T_UPDATE		15		//in milliseconds
#define MEAN_PKTSIZE	1500	//to be decided

#include <stdio.h>
#include <rte_pie.h>
#include <rte_random.h>
#include <rte_timer.h>

void rte_pie_config_init(struct rte_pie_config *pie_config){
	pie_config->burst_allowance = MAX_BURST;
	pie_config->drop_prob = 	0;
	pie_config->qdelay = 		0;
	pie_config->qdelay_old = 	0;
	pie_config->alpha = 		0.125;
	pie_config->beta = 			1.25;
}

/*//add synchronization
void rte_pie_set_current_qdelay(struct rte_pie_config *PIE, uint64_t timestamp){
	uint64_t now = rte_get_tsc_cycles();		//get the total number of cycles since boot.
	//uint64_t cycles_in_second = rte_get_timer_hz(); //get the number of cycles in one second
	//now /= cycles_in_second / 1000;	// get the timestamp in milliseconds.
	PIE->qdelay = now - timestamp;	//current queue delay in milliseconds.
}*/

/*
 * To ensure that PIE is "work conserving", we bypass the random drop if
 * the latency sample, PIE->qdelay_old_, is smaller than half of the
 * target latency value (QDELAY_REF) when the drop probability is not
 * too high (i.e., PIE->drop_prob_ < 0.2), or if the queue has less than
 * a couple of packets.
 */
int rte_pie_drop(struct rte_pie_config *pie_config, uint64_t qlen) {
	//Safeguard PIE to be work conserving
	if((pie_config->qdelay_old < QDELAY_REF/2 && pie_config->drop_prob < 0.2) || (qlen <= 2 * MEAN_PKTSIZE))
		return ENQUE;

	//function from rte_random.h return random number less than argument specified.
	uint64_t u = 15521u; //rte_rand_max(1 << 22);
	if((double) u < pie_config->drop_prob)
		return DROP;
	return ENQUE;
}


//Called on each packet arrival
int rte_pie_enque(struct rte_pie_config *pie, uint64_t qlen) {
	int current_qdelay = pie->qdelay;

	//burst allowance is multiple of t_update
	if (pie->burst_allowance == 0 && rte_pie_drop(pie, qlen) == DROP)
		return DROP;

	if (pie->drop_prob == 0 && current_qdelay < QDELAY_REF/2 && pie->qdelay_old < QDELAY_REF/2) {
			pie->burst_allowance = MAX_BURST;
	}
	return ENQUE;
}
