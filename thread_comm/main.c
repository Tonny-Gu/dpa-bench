#define _POSIX_C_SOURCE 200809L

#include <doca_dev.h>
#include <doca_dpa.h>
#include <doca_error.h>
#include <doca_sync_event.h>

#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "thread_comm_common.h"

struct thread_comm_app {
	struct doca_dev *dev;
	struct doca_dpa *dpa;
	doca_dpa_dev_t dpa_handle;
	struct doca_dpa_thread *thread_a;
	struct doca_dpa_thread *thread_b;
	struct doca_dpa_notification_completion *thread_a_notify;
	struct doca_dpa_notification_completion *thread_b_notify;
	struct doca_sync_event *host_sync_event;
	doca_dpa_dev_notification_completion_t thread_a_notify_handle;
	doca_dpa_dev_notification_completion_t thread_b_notify_handle;
	doca_dpa_dev_sync_event_t host_sync_event_handle;
	doca_dpa_dev_uintptr_t shared_state_dev_ptr;
	doca_dpa_dev_uintptr_t thread_a_arg_dev_ptr;
	doca_dpa_dev_uintptr_t thread_b_arg_dev_ptr;
	bool dpa_started;
	bool thread_a_started;
	bool thread_b_started;
	bool thread_a_notify_started;
	bool thread_b_notify_started;
	bool host_sync_event_started;
};

extern struct doca_dpa_app *dpa_thread_comm_app;

doca_dpa_func_t thread_a_kernel;
doca_dpa_func_t thread_b_kernel;
doca_dpa_func_t kick_thread_a_rpc;

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

static doca_error_t create_dpa_thread(struct thread_comm_app *app,
					      doca_dpa_func_t *func,
					      doca_dpa_dev_uintptr_t arg_dev_ptr,
					      struct doca_dpa_thread **thread,
					      bool *started)
{
	doca_error_t result;

	result = doca_dpa_thread_create(app->dpa, thread);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_dpa_thread_set_func_arg(*thread, func, arg_dev_ptr);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_dpa_thread_start(*thread);
	if (result != DOCA_SUCCESS)
		return result;

	*started = true;
	return DOCA_SUCCESS;
}

static doca_error_t create_notification_completion(struct thread_comm_app *app,
						   struct doca_dpa_thread *thread,
						   struct doca_dpa_notification_completion **notify_comp,
						   doca_dpa_dev_notification_completion_t *handle,
						   bool *started)
{
	doca_error_t result;

	result = doca_dpa_notification_completion_create(app->dpa, thread, notify_comp);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_dpa_notification_completion_start(*notify_comp);
	if (result != DOCA_SUCCESS)
		return result;

	*started = true;
	return doca_dpa_notification_completion_get_dpa_handle(*notify_comp, handle);
}

static doca_error_t create_completion_sync_event(struct thread_comm_app *app)
{
	doca_error_t result;

	result = doca_sync_event_create(&app->host_sync_event);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_sync_event_add_publisher_location_dpa(app->host_sync_event, app->dpa);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_sync_event_add_subscriber_location_cpu(app->host_sync_event, app->dev);
	if (result != DOCA_SUCCESS)
		return result;

	result = doca_sync_event_start(app->host_sync_event);
	if (result != DOCA_SUCCESS)
		return result;

	app->host_sync_event_started = true;
	return doca_sync_event_get_dpa_handle(app->host_sync_event, app->dpa, &app->host_sync_event_handle);
}

static doca_error_t thread_comm_app_destroy(struct thread_comm_app *app)
{
	doca_error_t result = DOCA_SUCCESS;
	doca_error_t tmp;

	if (app->thread_a_notify_started) {
		tmp = doca_dpa_notification_completion_stop(app->thread_a_notify);
		set_first_error(&result, tmp);
		app->thread_a_notify_started = false;
	}

	if (app->thread_b_notify_started) {
		tmp = doca_dpa_notification_completion_stop(app->thread_b_notify);
		set_first_error(&result, tmp);
		app->thread_b_notify_started = false;
	}

	if (app->thread_a_started) {
		tmp = doca_dpa_thread_stop(app->thread_a);
		set_first_error(&result, tmp);
		app->thread_a_started = false;
	}

	if (app->thread_b_started) {
		tmp = doca_dpa_thread_stop(app->thread_b);
		set_first_error(&result, tmp);
		app->thread_b_started = false;
	}

	if (app->thread_a_notify != NULL) {
		tmp = doca_dpa_notification_completion_destroy(app->thread_a_notify);
		set_first_error(&result, tmp);
		app->thread_a_notify = NULL;
	}

	if (app->thread_b_notify != NULL) {
		tmp = doca_dpa_notification_completion_destroy(app->thread_b_notify);
		set_first_error(&result, tmp);
		app->thread_b_notify = NULL;
	}

	if (app->thread_a != NULL) {
		tmp = doca_dpa_thread_destroy(app->thread_a);
		set_first_error(&result, tmp);
		app->thread_a = NULL;
	}

	if (app->thread_b != NULL) {
		tmp = doca_dpa_thread_destroy(app->thread_b);
		set_first_error(&result, tmp);
		app->thread_b = NULL;
	}

	if (app->host_sync_event_started) {
		tmp = doca_sync_event_stop(app->host_sync_event);
		set_first_error(&result, tmp);
		app->host_sync_event_started = false;
	}

	if (app->host_sync_event != NULL) {
		tmp = doca_sync_event_destroy(app->host_sync_event);
		set_first_error(&result, tmp);
		app->host_sync_event = NULL;
	}

	if (app->thread_a_arg_dev_ptr != 0) {
		tmp = doca_dpa_mem_free(app->dpa, app->thread_a_arg_dev_ptr);
		set_first_error(&result, tmp);
		app->thread_a_arg_dev_ptr = 0;
	}

	if (app->thread_b_arg_dev_ptr != 0) {
		tmp = doca_dpa_mem_free(app->dpa, app->thread_b_arg_dev_ptr);
		set_first_error(&result, tmp);
		app->thread_b_arg_dev_ptr = 0;
	}

	if (app->shared_state_dev_ptr != 0) {
		tmp = doca_dpa_mem_free(app->dpa, app->shared_state_dev_ptr);
		set_first_error(&result, tmp);
		app->shared_state_dev_ptr = 0;
	}

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

static doca_error_t thread_comm_app_init(struct thread_comm_app *app, const char *device_name)
{
	struct thread_comm_shared_state shared_state = {0};
	struct thread_b_arg thread_b_arg = {0};
	struct thread_a_arg thread_a_arg = {0};
	doca_error_t result;

	memset(app, 0, sizeof(*app));

	result = open_doca_device(device_name, &app->dev);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_dpa_create(app->dev, &app->dpa);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_dpa_set_app(app->dpa, dpa_thread_comm_app);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_dpa_start(app->dpa);
	if (result != DOCA_SUCCESS)
		goto fail;
	app->dpa_started = true;

	result = doca_dpa_get_dpa_handle(app->dpa, &app->dpa_handle);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_dpa_mem_alloc(app->dpa, sizeof(shared_state), &app->shared_state_dev_ptr);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_dpa_h2d_memcpy(app->dpa, app->shared_state_dev_ptr, &shared_state, sizeof(shared_state));
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_dpa_mem_alloc(app->dpa, sizeof(thread_a_arg), &app->thread_a_arg_dev_ptr);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_dpa_mem_alloc(app->dpa, sizeof(thread_b_arg), &app->thread_b_arg_dev_ptr);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = create_completion_sync_event(app);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = create_dpa_thread(app, &thread_b_kernel, app->thread_b_arg_dev_ptr, &app->thread_b, &app->thread_b_started);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = create_notification_completion(app,
						app->thread_b,
						&app->thread_b_notify,
						&app->thread_b_notify_handle,
						&app->thread_b_notify_started);
	if (result != DOCA_SUCCESS)
		goto fail;

	thread_b_arg.dpa_handle = app->dpa_handle;
	thread_b_arg.shared_state_dev_ptr = app->shared_state_dev_ptr;
	thread_b_arg.host_sync_event_handle = app->host_sync_event_handle;
	result = doca_dpa_h2d_memcpy(app->dpa, app->thread_b_arg_dev_ptr, &thread_b_arg, sizeof(thread_b_arg));
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_dpa_thread_run(app->thread_b);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = create_dpa_thread(app, &thread_a_kernel, app->thread_a_arg_dev_ptr, &app->thread_a, &app->thread_a_started);
	if (result != DOCA_SUCCESS)
		goto fail;

	result = create_notification_completion(app,
						app->thread_a,
						&app->thread_a_notify,
						&app->thread_a_notify_handle,
						&app->thread_a_notify_started);
	if (result != DOCA_SUCCESS)
		goto fail;

	thread_a_arg.dpa_handle = app->dpa_handle;
	thread_a_arg.shared_state_dev_ptr = app->shared_state_dev_ptr;
	thread_a_arg.thread_b_notify_handle = app->thread_b_notify_handle;
	result = doca_dpa_h2d_memcpy(app->dpa, app->thread_a_arg_dev_ptr, &thread_a_arg, sizeof(thread_a_arg));
	if (result != DOCA_SUCCESS)
		goto fail;

	result = doca_dpa_thread_run(app->thread_a);
	if (result != DOCA_SUCCESS)
		goto fail;

	return DOCA_SUCCESS;

fail:
	(void)thread_comm_app_destroy(app);
	return result;
}

int main(int argc, char **argv)
{
	const char *device_name;
	struct thread_comm_app app;
	struct thread_comm_shared_state shared_state = {0};
	uint64_t rpc_retval = 0;
	doca_error_t result;
	doca_error_t cleanup_result;
	int exit_code = EXIT_SUCCESS;

	if (parse_args(argc, argv, &device_name) != 0) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	result = thread_comm_app_init(&app, device_name);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "thread_comm_app_init failed: %s\n", doca_error_get_descr(result));
		return EXIT_FAILURE;
	}

	printf("Running DPA thread communication example on %s\n",
	       device_name[0] != '\0' ? device_name : "first DPA-capable device");

	result = doca_dpa_rpc(app.dpa,
			      &kick_thread_a_rpc,
			      &rpc_retval,
			      (uint64_t)app.dpa_handle,
			      (uint64_t)app.thread_a_notify_handle);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "doca_dpa_rpc failed: %s\n", doca_error_get_descr(result));
		exit_code = EXIT_FAILURE;
		goto out;
	}

	result = doca_sync_event_wait_gt(app.host_sync_event, 0, UINT64_MAX);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "doca_sync_event_wait_gt failed: %s\n", doca_error_get_descr(result));
		exit_code = EXIT_FAILURE;
		goto out;
	}

	result = doca_dpa_d2h_memcpy(app.dpa, &shared_state, app.shared_state_dev_ptr, sizeof(shared_state));
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "doca_dpa_d2h_memcpy failed: %s\n", doca_error_get_descr(result));
		exit_code = EXIT_FAILURE;
		goto out;
	}

	result = doca_dpa_peek_at_last_error(app.dpa);
	if (result != DOCA_SUCCESS) {
		fprintf(stderr, "DPA runtime error: %s\n", doca_error_get_descr(result));
		exit_code = EXIT_FAILURE;
		goto out;
	}

	printf("Shared state after thread A -> thread B handoff:\n");
	printf("  stage   = %llu\n", (unsigned long long)shared_state.stage);
	printf("  message = 0x%llx\n", (unsigned long long)shared_state.message);
	printf("  reply   = 0x%llx\n", (unsigned long long)shared_state.reply);

	if (shared_state.stage != THREAD_COMM_STAGE_THREAD_B ||
	    shared_state.message != THREAD_COMM_MESSAGE ||
	    shared_state.reply != THREAD_COMM_REPLY) {
		fprintf(stderr, "Unexpected thread communication result\n");
		exit_code = EXIT_FAILURE;
		goto out;
	}

	printf("Communication succeeded: thread A wrote shared memory and thread B consumed it.\n");

out:
	cleanup_result = thread_comm_app_destroy(&app);
	if (cleanup_result != DOCA_SUCCESS) {
		fprintf(stderr, "thread_comm_app_destroy failed: %s\n", doca_error_get_descr(cleanup_result));
		exit_code = EXIT_FAILURE;
	}

	return exit_code;
}
