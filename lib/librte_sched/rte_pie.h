#include <stdio.h>

#define QDELAY_REF 	15	//in milliseconds
#define MAX_BURST 	150	//in milliseconds
#define DQ_THRESHOLD 1 << 14	//16KB
#define T_UPDATE	15	//in milliseconds
#define TAIL_DROP	-------------

double alpha = 0.125;
double beta = 1.25;

struct rte_pie_status {
	double burst_allowance;
	double drop_prob;
	double accu_prob;
	double qdelay_old;
	long last_timestamp;
};

struct rte_pie_params {
	double burst_allowance;
};

void deque(Packet packet);
int calculate_drop_prob();
int drop_early();
int enque(Packet packet);

/*
 * static int rte_pie_init(struct rte_pie);
 * static int rte_pie_drop_early(struct rte_pie);
 * static int rte_pie_drop(struct rte_pie);
 * static int rte_pie_calculate_drop_prob(struct rte_pie);
 * static int rte_pie_enqueue(struct rte_pie)
 *
 */
