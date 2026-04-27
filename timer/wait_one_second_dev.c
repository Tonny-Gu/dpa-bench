#include <doca_pcc_dev_utils.h>

#define DPA_WAIT_US 1000000ULL

__dpa_rpc__ uint64_t dpa_wait_one_second_rpc(void)
{
	uint64_t start = doca_pcc_dev_get_timer();

	while (doca_pcc_dev_get_timer() - start < DPA_WAIT_US)
		;

	return doca_pcc_dev_get_timer() - start;
}
