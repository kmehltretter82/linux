#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/kcov.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#define WORKERS 4
#define ITERATIONS 2000
#define CHUNK (16 * 1024)
#define KCOV_ENTRIES (1 << 16)
#define KCOV_BYTES (KCOV_ENTRIES * sizeof(unsigned long))

static pthread_barrier_t start_barrier;
static atomic_int failures;

static int kcov_start(unsigned long **area, int *fd, int id)
{
	unsigned long *map;
	int kfd, saved_errno;

	kfd = open("/sys/kernel/debug/kcov", O_RDWR);
	if (kfd < 0) {
		dprintf(STDERR_FILENO, "worker %d: KCOV open: %s\n", id,
			strerror(errno));
		return -1;
	}
	if (ioctl(kfd, KCOV_INIT_TRACE, KCOV_ENTRIES) < 0) {
		dprintf(STDERR_FILENO, "worker %d: KCOV init: %s\n", id,
			strerror(errno));
		goto fail_close;
	}
	map = mmap(NULL, KCOV_BYTES,
			PROT_READ | PROT_WRITE, MAP_SHARED, kfd, 0);
	if (map == MAP_FAILED) {
		dprintf(STDERR_FILENO, "worker %d: KCOV mmap: %s\n", id,
			strerror(errno));
		goto fail_close;
	}
	if (ioctl(kfd, KCOV_ENABLE, KCOV_TRACE_PC) < 0) {
		dprintf(STDERR_FILENO, "worker %d: KCOV enable: %s\n", id,
			strerror(errno));
		saved_errno = errno;
		munmap(map, KCOV_BYTES);
		errno = saved_errno;
		goto fail_close;
	}
	*area = map;
	*fd = kfd;
	return 0;

fail_close:
	saved_errno = errno;
	close(kfd);
	errno = saved_errno;
	return -1;
}

static void *worker(void *arg)
{
	unsigned long *area;
	unsigned char *source, *destination;
	int pipefd[2], kfd, id = (int)(uintptr_t)arg;

	if (kcov_start(&area, &kfd, id) < 0)
		exit(EXIT_FAILURE);
	if (pipe2(pipefd, O_CLOEXEC) < 0) {
		dprintf(STDERR_FILENO, "worker %d: pipe failed: %s\n", id,
			strerror(errno));
		exit(EXIT_FAILURE);
	}
	source = aligned_alloc(64, CHUNK);
	destination = aligned_alloc(64, CHUNK);
	if (!source || !destination) {
		dprintf(STDERR_FILENO, "worker %d: allocation failed: %s\n", id,
			strerror(errno));
		free(source);
		free(destination);
		exit(EXIT_FAILURE);
	}
	for (size_t i = 0; i < CHUNK; i++)
		source[i] = (unsigned char)(0x30 + id + i);
	pthread_barrier_wait(&start_barrier);

	for (int i = 0; i < ITERATIONS; i++) {
		size_t done = 0;

		while (done < CHUNK) {
			ssize_t n = write(pipefd[1], source + done, CHUNK - done);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0) {
				atomic_fetch_add(&failures, 1);
				goto out_buffers;
			}
			done += n;
		}
		done = 0;
		memset(destination, 0xa5, CHUNK);
		while (done < CHUNK) {
			ssize_t n = read(pipefd[0], destination + done, CHUNK - done);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0) {
				atomic_fetch_add(&failures, 1);
				goto out_buffers;
			}
			done += n;
		}
		if (memcmp(source, destination, CHUNK)) {
			dprintf(STDERR_FILENO, "worker %d: data mismatch\n", id);
			atomic_fetch_add(&failures, 1);
			goto out_buffers;
		}
		if ((i & 7) == 0)
			sched_yield();
	}

out_buffers:
	free(destination);
	free(source);
	close(pipefd[0]);
	close(pipefd[1]);
	ioctl(kfd, KCOV_DISABLE, 0);
	munmap(area, KCOV_BYTES);
	close(kfd);
	return NULL;
}

int main(void)
{
	pthread_t threads[WORKERS];
	int ret;

	ret = pthread_barrier_init(&start_barrier, NULL, WORKERS);
	if (ret) {
		errno = ret;
		perror("pthread_barrier_init");
		return EXIT_FAILURE;
	}
	for (int i = 0; i < WORKERS; i++) {
		ret = pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i);
		if (ret) {
			errno = ret;
			perror("pthread_create");
			return EXIT_FAILURE;
		}
	}
	for (int i = 0; i < WORKERS; i++)
		pthread_join(threads[i], NULL);
	pthread_barrier_destroy(&start_barrier);

	printf("vector-stress complete failures=%d\n", atomic_load(&failures));
	return atomic_load(&failures) ? 1 : 0;
}
