/* registry.c - the one definition of conf_cases[]. Order here is run order and result order.
 *
 * CONF_CASES lists every case once: X(group, name, flags). The case function is
 * case_<group>_<name> (defined in cases/<group>.c), the id is "<group>.<name>". Group and name
 * are plain identifiers in lower case, so the id keeps the [a-z0-9_.] grammar of FORMAT.md.
 * A name that is missing here or defined twice is a link error, not a silent omission.
 */
#include "../conf.h"

#define CONF_CASES(X) \
	X(time, timebase_freq, 0) \
	X(time, tb_monotonic, 0) \
	X(time, current_time, 0) \
	X(time, current_time_monotonic, 0) \
	X(time, current_time_null_sec, 0) \
	X(time, current_time_null_nsec, 0) \
	X(time, timezone, 0) \
	X(mutex, create_destroy, 0) \
	X(mutex, create_two_distinct, 0) \
	X(mutex, object_count, 0) \
	X(mutex, create_protocols, 0) \
	X(mutex, create_recursive_values, 0) \
	X(mutex, create_pshared_values, 0) \
	X(mutex, create_adaptive_values, 0) \
	X(mutex, create_null_id, 0) \
	X(mutex, create_null_attr, 0) \
	X(mutex, lock_unlock, 0) \
	X(mutex, lock_relock_nonrecursive, 0) \
	X(mutex, trylock_relock_nonrecursive, 0) \
	X(mutex, recursive_count, 0) \
	X(mutex, unlock_not_owner, 0) \
	X(mutex, destroy_locked, 0) \
	X(mutex, destroy_twice, 0) \
	X(mutex, invalid_id, 0) \
	X(cond, create_destroy, 0) \
	X(cond, object_count, 0) \
	X(cond, create_bad_mutex, 0) \
	X(cond, create_pshared_values, 0) \
	X(cond, mutex_destroy_with_cond, 0) \
	X(cond, destroy_twice, 0) \
	X(cond, invalid_id, 0) \
	X(cond, signal_no_waiter, 0) \
	X(cond, signal_to_bogus_thread, 0) \
	X(cond, wait_not_owner, 0) \
	X(cond, wait_timeout, 0) \
	X(cond, wait_timeout_recursive, 0) \
	X(semaphore, create_destroy, 0) \
	X(semaphore, object_count, 0) \
	X(semaphore, create_params, 0) \
	X(semaphore, create_protocols, 0) \
	X(semaphore, create_pshared_values, 0) \
	X(semaphore, create_null_id, 0) \
	X(semaphore, create_null_attr, 0) \
	X(semaphore, trywait_zero, 0) \
	X(semaphore, trywait_counts_down, 0) \
	X(semaphore, wait, 0) \
	X(semaphore, post_get_value, 0) \
	X(semaphore, post_past_max, 0) \
	X(semaphore, post_bad_count, 0) \
	X(semaphore, get_value_null, 0) \
	X(semaphore, invalid_id, 0) \
	X(semaphore, destroy_twice, 0) \
	X(rwlock, create_destroy, 0) \
	X(rwlock, object_count, 0) \
	X(rwlock, create_protocols, 0) \
	X(rwlock, create_null_args, 0) \
	X(rwlock, readers_stack, 0) \
	X(rwlock, trywlock_exclusive, 0) \
	X(rwlock, trywlock_slot_148, 0) \
	X(rwlock, wlock_relock, 0) \
	X(rwlock, rlock_under_writer, 0) \
	X(rwlock, wlock_under_reader, 0) \
	X(rwlock, trywlock_under_reader, 0) \
	X(rwlock, runlock_not_held, 0) \
	X(rwlock, wunlock_not_held, 0) \
	X(rwlock, destroy_locked, 0) \
	X(rwlock, invalid_id, 0) \
	X(event, queue_create_destroy, 0) \
	X(event, queue_object_count, 0) \
	X(event, queue_create_sizes, 0) \
	X(event, queue_create_protocols, 0) \
	X(event, queue_create_types, 0) \
	X(event, queue_destroy_modes, 0) \
	X(event, queue_destroy_invalid, 0) \
	X(event, queue_tryreceive_empty, 0) \
	X(event, queue_tryreceive_errors, 0) \
	X(event, queue_receive_timeout, 0) \
	X(event, queue_receive_errors, 0) \
	X(event, queue_drain, 0) \
	X(event, queue_destroy_with_events, 0) \
	X(event, port_create_destroy, 0) \
	X(event, port_create_types, 0) \
	X(event, port_destroy_invalid, 0) \
	X(event, port_connect_disconnect, 0) \
	X(event, port_connect_invalid, 0) \
	X(event, port_connect_wrong_type, 0) \
	X(event, port_destroy_connected, 0) \
	X(event, port_send_errors, 0) \
	X(event, port_send_payload, 0) \
	X(event, port_send_default_source, 0) \
	X(event, port_send_full_queue, 0) \
	X(event, port_send_order, 0) \
	X(event, port_outlives_queue, 0) \
	X(event, queue_send_then_receive, 0) \
	X(eventflag, create_destroy, 0) \
	X(eventflag, object_count, 0) \
	X(eventflag, create_protocols, 0) \
	X(eventflag, create_types, 0) \
	X(eventflag, create_null_args, 0) \
	X(eventflag, set_clear_get, 0) \
	X(eventflag, pattern_64bit, 0) \
	X(eventflag, trywait_and, 0) \
	X(eventflag, trywait_or, 0) \
	X(eventflag, trywait_clear, 0) \
	X(eventflag, trywait_clear_all, 0) \
	X(eventflag, trywait_clear_on_failure, 0) \
	X(eventflag, bad_modes, 0) \
	X(eventflag, wait_ready, 0) \
	X(eventflag, wait_timeout, 0) \
	X(eventflag, cancel_no_waiters, 0) \
	X(eventflag, get_null, 0) \
	X(eventflag, invalid_id, 0) \
	X(eventflag, destroy_twice, 0) \
	X(memory, allocate_1m, 0) \
	X(memory, allocate_64k, 0) \
	X(memory, allocate_default_flags, 0) \
	X(memory, allocate_bad_flags, 0) \
	X(memory, allocate_size_zero, 0) \
	X(memory, allocate_size_alignment, 0) \
	X(memory, allocate_too_big, 0) \
	X(memory, allocate_null_addr, 0) \
	X(memory, free_errors, 0) \
	X(memory, user_memory_size, 0) \
	X(memory, allocate_changes_available, 0) \
	X(memory, allocate_64k_changes_available, 0) \
	X(memory, page_attribute, 0) \
	X(memory, page_attribute_64k, 0) \
	X(memory, page_attribute_unmapped, 0) \
	X(container, create_destroy, 0) \
	X(container, create_destroy_old_slots, 0) \
	X(container, slot_cross, 0) \
	X(container, create_several, 0) \
	X(container, create_sizes, 0) \
	X(container, create_too_big, 0) \
	X(container, changes_user_memory, 0) \
	X(container, invalid_id, 0) \
	X(container, allocate_from, 0) \
	X(container, allocate_exhaust, 0) \
	X(container, allocate_from_errors, 0) \
	X(container, neighbour_slots, 0) \
	X(process, getpid_stable, 0) \
	X(process, getppid, 0) \
	X(process, get_sdk_version, 0) \
	X(process, object_count, 0) \
	X(process, get_id_lists_mutex, 0) \
	X(process, get_id_unknown_type, 0) \
	X(ppu_thread, yield, 0) \
	X(ppu_thread, get_priority_unknown, 0) \
	X(ppu_thread, set_priority_checks, 0) \
	X(ppu_thread, get_join_state, 0) \
	X(ppu_thread, get_stack_information, 0) \
	X(ppu_thread, join_detach_unknown, 0) \
	X(timer, create_destroy, 0) \
	X(timer, destroy_invalid, 0) \
	X(timer, start_unconnected, 0) \
	X(timer, start_checks, 0) \
	X(timer, connect_disconnect, 0) \
	X(timer, connect_invalid, 0) \
	X(timer, start_oneshot_in_past, 0) \
	X(timer, get_information, 0) \
	X(timer, get_information_invalid, 0) \
	X(timer, usleep_returns, CONF_TIMING) \
	X(timer, usleep_duration, CONF_TIMING) \
	X(timer, usleep_zero, 0) \
	X(timer, sleep_zero, 0) \
	X(slot96, spu_image_close_zeroed, 0) \
	X(slot96, spu_initialize_twice, 0) \
	X(slot96, spu_group_unknown, 0) \
	X(slot96, spu_thread_unknown, 0) \
	X(slot96, fs_fsync_bad_fd, 0) \
	X(slot96, fs_ftruncate_bad_fd, 0) \
	X(threads, create_join_value, CONF_THREADS) \
	X(threads, self_join, CONF_THREADS) \
	X(threads, priority_of_child, CONF_THREADS) \
	X(threads, self_priority, CONF_THREADS) \
	X(threads, join_state_by_flags, CONF_THREADS) \
	X(threads, detach_errors, CONF_THREADS) \
	X(threads, mutex_contention, CONF_THREADS) \
	X(threads, mutex_destroy_with_waiter, CONF_THREADS) \
	X(threads, semaphore_waiter, CONF_THREADS) \
	X(threads, semaphore_timeout_in_child, CONF_THREADS) \
	X(threads, cond_signal, CONF_THREADS) \
	X(threads, cond_destroy_with_waiter, CONF_THREADS) \
	X(threads, event_receive_wakeup, CONF_THREADS) \
	X(threads, event_queue_destroy_with_receiver, CONF_THREADS) \
	X(threads, eventflag_set_wakes, CONF_THREADS) \
	X(threads, eventflag_cancel, CONF_THREADS) \
	X(threads, eventflag_single_waiter, CONF_THREADS) \
	X(threads, rwlock_reader_blocks_writer, CONF_THREADS)

#define CONF_DECL(g, n, f) void case_##g##_##n(void);
CONF_CASES(CONF_DECL)
#undef CONF_DECL

#define CONF_ENTRY(g, n, f) { #g "." #n, #g, (f), case_##g##_##n },
const struct conf_case conf_cases[] = {
	CONF_CASES(CONF_ENTRY)
};
#undef CONF_ENTRY

const unsigned conf_case_count = (unsigned)(sizeof(conf_cases) / sizeof(conf_cases[0]));
