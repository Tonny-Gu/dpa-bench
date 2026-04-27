#define _POSIX_C_SOURCE 200809L

#include <doca_dev.h>
#include <doca_dpa.h>
#include <doca_error.h>

#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct timer_app {
	struct doca_dev *dev;
	struct doca_dpa *dpa;
	bool dpa_started;
};

extern struct doca_dpa_app *dpa_timer_app;

doca_dpa_func_t dpa_wait_one_second_rpc;

static double now_us(void)
{
	struct timespec ts = {0};

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (double)ts.tv_sec * 1000000.0 + (double)ts.tv_nsec / 1000.0;
}

static void set_first_error(doca_error_t *result, doca_error_t err)
{
	if (*result == DOCA_SUCCESS && err != DOCA_SUCCESS)
		*result = err;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [--device <ib_hca>]\n"
		"  --device <ib_hca>  Use a specific DPA-capable device\n",
		prog);
}

static int parse_args(int argc, char **argv, const char **device_name)
{
	static const struct option long_opts[] = {
		{"device", required_argument, NULL, 'd'},
		{0, 0, 0, 0},
	};
	int opt;

	*device_name = "";

	while ((opt = getopt_long(argc, argv, "d:", long_opts, NULL)) != -1) {
		switch (opt) {
		case 'd':
			*device_name = optarg;
			break;
		default:
			return -1;
		}
	}

	return optind == argc ? 0 : -1;
}

static doca_error_t open_doca_device(const char *device_name, struct doca_dev **dev)
{
	struct doca_devinfo **dev_list = NULL;
	uint32_t nb_devs = 0;
	char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};
	const char *wanted = device_name == NULL ? "" : device_name;
	doca_error_t result;
	uint32_t i;

	*dev = NULL;
	result = doca_devinfo_create_list(&dev_list, &nb_devs);
	if (result != DOCA_SUCCESS)
		return result;

	for (i = 0; i < nb_devs; ++i) {
		result = doca_devinfo_get_ibdev_name(dev_list[i], ibdev_name, sizeof(ibdev_name));
		if (result != DOCA_SUCCESS)
			continue;
		if (wanted[0] != '\0' && strncmp(wanted, ibdev_name, sizeof(ibdev_name)) != 0)
			continue;
		if (doca_dpa_cap_is_supported(dev_list[i]) != DOCA_SUCCESS)
			continue;
		result = doca_dev_open(dev_list[i], dev);
		if (result == DOCA_SUCCESS)
			break;
	}

	doca_devinfo_destroy_list(dev_list);
	if (*dev == NULL)
		return DOCA_ERROR_NOT_FOUND;
	return DOCA_SUCCESS;
}

static doca_error_t timer_app_destroy(struct timer_app *app)
{
	doca_error_t result = DOCA_SUCCESS;
	doca_error_t tmp;

	if (app->dpa_started) {
		tmp = doca_dpa_stop(app->dpa);
		set_first_error(&result, tmp);
		app->dpa_started = false;
	}

	if (app->dpa != NULL) {
		tmp = doca_dpa_destroy(app->dpa);
		set_first_error(&result, tmp);
		app->dpa = NULL;
	}

	if (app->dev != NULL) {
		tmp = doca_dev_close(app->dev);
		set_first_error(&result, tmp);
		app->dev = NULL;
	}

	return result;
}

static doca_error_t timer_app_init(struct timer_app *app, const char *device_name)
{
	doca_error_t result;

	memset(app, 0, sizeof(*app));

	result = open_doca_device(device_name, &app->dev);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_dpa_create(app->dev, &app->dpa);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_dpa_set_app(app->dpa, dpa_timer_app);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_dpa_start(app->dpa);
	if (result != DOCA_SUCCESS)
		goto fail;

	app->dpa_started = true;
	return DOCA_SUCCESS;

fail:
	(void)timer_app_destroy(app);
	return result;
}

int main(int argc, char **argv)
{
	const char *device_name;
	struct timer_app app;
	unsigned long long max_run_time_s = 0;
	double host_start_us;
	double host_elapsed_us;
	uint64_t dpa_elapsed_us = 0;
	doca_error_t result;
	doca_error_t cleanup_result;
	int exit_code = EXIT_SUCCESS;

	if (parse_args(argc, argv, &device_name) != 0) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	result = timer_app_init(&app, device_name);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "timer_app_init failed: %s\n", doca_error_get_descr(result));
		return EXIT_FAILURE;
	}

	result = doca_dpa_get_kernel_max_run_time(app.dpa, &max_run_time_s);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "doca_dpa_get_kernel_max_run_time failed: %s\n", doca_error_get_descr(result));
		exit_code = EXIT_FAILURE;
		goto out;
	}

	printf("Running 1-second DPA wait on %s\n",
	       device_name[0] != '\0' ? device_name : "first DPA-capable device");
	printf("DPA kernel max run time: %llu s\n", max_run_time_s);

	host_start_us = now_us();
	result = doca_dpa_rpc(app.dpa, &dpa_wait_one_second_rpc, &dpa_elapsed_us);
	host_elapsed_us = now_us() - host_start_us;
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "doca_dpa_rpc failed: %s\n", doca_error_get_descr(result));
		exit_code = EXIT_FAILURE;
		goto out;
	}

	result = doca_dpa_peek_at_last_error(app.dpa);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "DPA runtime error: %s\n", doca_error_get_descr(result));
		exit_code = EXIT_FAILURE;
		goto out;
	}

	printf("DPA reported wait: %" PRIu64 " us\n", dpa_elapsed_us);
	printf("Host observed RPC time: %.0f us\n", host_elapsed_us);

out:
	cleanup_result = timer_app_destroy(&app);
	if (cleanup_result != DOCA_SUCCESS) {
		fprintf(stderr, "timer_app_destroy failed: %s\n", doca_error_get_descr(cleanup_result));
		exit_code = EXIT_FAILURE;
	}

	return exit_code;
}
