#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct mmap_result {
	int succeeded;
	int error;
};

static void fail(const char *what)
{
	perror(what);
	exit(EXIT_FAILURE);
}

static void write_all(int fd, const void *buffer, size_t length)
{
	const char *p = buffer;

	while (length) {
		ssize_t written = write(fd, p, length);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			fail("write");
		}
		p += written;
		length -= (size_t)written;
	}
}

static void read_all_timeout(int fd, void *buffer, size_t length,
		const char *what)
{
	char *p = buffer;

	while (length) {
		struct pollfd pollfd = { .fd = fd, .events = POLLIN };
		int ret = poll(&pollfd, 1, 30000);

		if (ret == 0) {
			fprintf(stderr, "timeout: %s\n", what);
			exit(EXIT_FAILURE);
		}
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			fail("poll");
		}

		ret = (int)read(fd, p, length);
		if (ret <= 0) {
			if (ret < 0 && errno == EINTR)
				continue;
			fprintf(stderr, "short read: %s\n", what);
			exit(EXIT_FAILURE);
		}
		p += ret;
		length -= (size_t)ret;
	}
}

static long read_number(const char *path)
{
	char buffer[64];
	char *end;
	ssize_t length;
	long value;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		fail(path);
	length = read(fd, buffer, sizeof(buffer) - 1);
	if (length <= 0)
		fail(path);
	close(fd);
	buffer[length] = '\0';
	errno = 0;
	value = strtol(buffer, &end, 10);
	if (errno || end == buffer) {
		fprintf(stderr, "invalid number in %s: %s\n", path, buffer);
		exit(EXIT_FAILURE);
	}
	return value;
}

static void debug_path(char *path, size_t size, const char *directory,
		const char *name)
{
	if (snprintf(path, size, "%s/%s", directory, name) >= (int)size) {
		fprintf(stderr, "debugfs path too long\n");
		exit(EXIT_FAILURE);
	}
}

static void write_number(const char *directory, const char *name, long value)
{
	char path[512];
	char buffer[64];
	int length;
	int fd;

	debug_path(path, sizeof(path), directory, name);
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		fail(path);
	length = snprintf(buffer, sizeof(buffer), "%ld\n", value);
	write_all(fd, buffer, (size_t)length);
	close(fd);
}

static long read_debug_number(const char *directory, const char *name)
{
	char path[512];

	debug_path(path, sizeof(path), directory, name);
	return read_number(path);
}

static void wait_for_stage(const char *directory, long expected)
{
	struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000 };
	int i;

	for (i = 0; i < 6000; i++) {
		long stage = read_debug_number(directory, "stage");

		if (stage == expected)
			return;
		if (stage > expected) {
			fprintf(stderr, "stage advanced unexpectedly: %ld > %ld\n",
				stage, expected);
			exit(EXIT_FAILURE);
		}
		nanosleep(&delay, NULL);
	}
	fprintf(stderr, "timeout waiting for stage %ld\n", expected);
	exit(EXIT_FAILURE);
}

static long pool_snapshot(const char *pool, const char *stage)
{
	char path[512];
	long total, free, reserved, surplus;

#define READ_POOL_FILE(member, destination) do { \
	if (snprintf(path, sizeof(path), "%s/%s", pool, member) >= \
	    (int)sizeof(path)) { \
		fprintf(stderr, "pool path too long\n"); \
		exit(EXIT_FAILURE); \
	} \
	destination = read_number(path); \
} while (0)

	READ_POOL_FILE("nr_hugepages", total);
	READ_POOL_FILE("free_hugepages", free);
	READ_POOL_FILE("resv_hugepages", reserved);
	READ_POOL_FILE("surplus_hugepages", surplus);
	printf("EARLYPROOF-POOL stage=%s total=%ld free=%ld reserved=%ld surplus=%ld\n",
	       stage, total, free, reserved, surplus);
#undef READ_POOL_FILE
	return reserved;
}

static int open_test_file(const char *path, off_t size)
{
	int fd = open(path, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);

	if (fd < 0)
		fail(path);
	if (ftruncate(fd, size))
		fail("ftruncate initial size");
	return fd;
}

static void *reserve_mapping(int fd, size_t length, const char *what)
{
	void *mapping;

	errno = 0;
	mapping = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	printf("EARLYPROOF-RESERVE name=%s success=%d errno=%d bytes=%zu\n",
	       what, mapping != MAP_FAILED, mapping == MAP_FAILED ? errno : 0,
	       length);
	return mapping;
}

static pid_t start_target_child(int fd, size_t length, int command[2],
		int status[2])
{
	pid_t pid = fork();

	if (pid < 0)
		fail("fork target child");
	if (pid == 0) {
		struct mmap_result result;
		char command_byte;
		void *mapping;

		close(command[1]);
		close(status[0]);
		read_all_timeout(command[0], &command_byte, 1,
				 "target-child command");
		if (command_byte != 'G')
			_exit(111);
		errno = 0;
		mapping = mmap(NULL, length, PROT_READ | PROT_WRITE,
			       MAP_SHARED, fd, 0);
		result.succeeded = mapping != MAP_FAILED;
		result.error = result.succeeded ? 0 : errno;
		if (result.succeeded)
			munmap(mapping, length);
		write_all(status[1], &result, sizeof(result));
		_exit(0);
	}
	close(command[0]);
	close(status[1]);
	return pid;
}

static void wait_child_ok(pid_t pid, const char *name)
{
	int status;

	if (waitpid(pid, &status, 0) != pid)
		fail("waitpid");
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "%s exited abnormally: status=%d\n", name, status);
		exit(EXIT_FAILURE);
	}
}

int main(int argc, char **argv)
{
	const int hold_pages = 2;
	const int target_pages = 4;
	const int first_steal_pages = 3;
	const int second_steal_pages = 2;
	const char *target_path, *hold_path, *first_steal_path;
	const char *second_steal_path, *pool, *debug_dir;
	int target_command[2], target_status[2];
	struct mmap_result target_result;
	void *hold_mapping = MAP_FAILED;
	void *first_steal_mapping = MAP_FAILED;
	void *second_steal_mapping = MAP_FAILED;
	long hpage_long, reserved;
	size_t hpage;
	pid_t target_pid = -1;
	char byte = 'G';
	int target_fd, hold_fd, first_steal_fd, second_steal_fd;
	int result = EXIT_FAILURE;

	if (argc != 8) {
		fprintf(stderr, "usage: %s TARGET HOLD STEAL1 STEAL2 HPAGE POOL DEBUG_DIR\n",
			argv[0]);
		return EXIT_FAILURE;
	}
	target_path = argv[1];
	hold_path = argv[2];
	first_steal_path = argv[3];
	second_steal_path = argv[4];
	hpage_long = strtol(argv[5], NULL, 10);
	pool = argv[6];
	debug_dir = argv[7];
	if (hpage_long <= 0 || (uint64_t)hpage_long > SIZE_MAX / target_pages) {
		fprintf(stderr, "invalid huge page size\n");
		return EXIT_FAILURE;
	}
	hpage = (size_t)hpage_long;

	target_fd = open_test_file(target_path, (off_t)(target_pages * hpage));
	hold_fd = open_test_file(hold_path, (off_t)(hold_pages * hpage));
	first_steal_fd = open_test_file(first_steal_path,
					(off_t)(first_steal_pages * hpage));
	second_steal_fd = open_test_file(second_steal_path,
					 (off_t)(second_steal_pages * hpage));

	hold_mapping = reserve_mapping(hold_fd, hold_pages * hpage, "hold");
	if (hold_mapping == MAP_FAILED)
		goto out;
	pool_snapshot(pool, "hold-reserved");

	if (pipe(target_command) || pipe(target_status))
		fail("pipe");
	target_pid = start_target_child(target_fd, target_pages * hpage,
					target_command, target_status);
	write_number(debug_dir, "release", 0);
	write_number(debug_dir, "fail_once", 0);
	write_number(debug_dir, "target_pid", target_pid);
	printf("EARLYPROOF-START target_pid=%d\n", target_pid);
	write_all(target_command[1], &byte, 1);

	wait_for_stage(debug_dir, 1);
	pool_snapshot(pool, "pause1-before-fill");
	first_steal_mapping = reserve_mapping(first_steal_fd,
			first_steal_pages * hpage, "fill-before-account");
	if (first_steal_mapping == MAP_FAILED)
		goto out_release;
	pool_snapshot(pool, "pause1-after-fill");
	write_number(debug_dir, "release", 1);

	wait_for_stage(debug_dir, 2);
	pool_snapshot(pool, "pause2-after-account-failure");
	if (ftruncate(hold_fd, 0)) {
		perror("release hold reservation");
		goto out_release;
	}
	pool_snapshot(pool, "pause2-hold-released");
	second_steal_mapping = reserve_mapping(second_steal_fd,
			second_steal_pages * hpage, "consume-released-capacity");
	if (second_steal_mapping == MAP_FAILED)
		goto out_release;
	pool_snapshot(pool, "pause2-after-second-fill");
	write_number(debug_dir, "release", 2);

	wait_for_stage(debug_dir, 3);
	read_all_timeout(target_status[0], &target_result, sizeof(target_result),
			 "target mmap result");
	wait_child_ok(target_pid, "target child");
	target_pid = -1;
	reserved = pool_snapshot(pool, "rollback-complete");
	printf("EARLYPROOF-TARGET success=%d errno=%d fail_hits=%ld timed_out=%ld stage=%ld reserved=%ld\n",
	       target_result.succeeded, target_result.error,
	       read_debug_number(debug_dir, "fail_hits"),
	       read_debug_number(debug_dir, "timed_out"),
	       read_debug_number(debug_dir, "stage"), reserved);
	if (target_result.succeeded || target_result.error != ENOMEM ||
	    read_debug_number(debug_dir, "fail_hits") != 0 ||
	    read_debug_number(debug_dir, "timed_out") != 0 || reserved != 7)
		goto out_release;
	printf("EARLYPROOF-ASSERT positive-correction-failed delta=2 visible_reserved=7 result=PASS\n");
	result = EXIT_SUCCESS;

out_release:
	write_number(debug_dir, "release", 2);
	if (target_pid > 0) {
		kill(target_pid, SIGKILL);
		waitpid(target_pid, NULL, 0);
	}
out:
	if (second_steal_mapping != MAP_FAILED)
		munmap(second_steal_mapping, second_steal_pages * hpage);
	if (first_steal_mapping != MAP_FAILED)
		munmap(first_steal_mapping, first_steal_pages * hpage);
	if (hold_mapping != MAP_FAILED)
		munmap(hold_mapping, hold_pages * hpage);
	if (ftruncate(target_fd, 0) || ftruncate(hold_fd, 0) ||
	    ftruncate(first_steal_fd, 0) || ftruncate(second_steal_fd, 0)) {
		perror("truncate during cleanup");
		result = EXIT_FAILURE;
	}
	close(target_fd);
	close(hold_fd);
	close(first_steal_fd);
	close(second_steal_fd);
	unlink(target_path);
	unlink(hold_path);
	unlink(first_steal_path);
	unlink(second_steal_path);
	reserved = pool_snapshot(pool, "files-removed");
	if (result == EXIT_SUCCESS && reserved != 2) {
		fprintf(stderr, "expected two backed minimum reservations, got %ld\n",
			reserved);
		result = EXIT_FAILURE;
	}
	return result;
}
