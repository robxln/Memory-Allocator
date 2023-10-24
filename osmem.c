// SPDX-License-Identifier: BSD-3-Clause

#include "osmem.h"
#include "helpers.h"

#define ALIGNMENT 8
#define ALIGN(size) (((size) + (ALIGNMENT-1)) & ~(ALIGNMENT-1))
#define BLOCK_META_SIZE sizeof(struct block_meta)
#define PREALLOC_SIZE (128 * 1024) // 128kb
#define ALLOCATION_FAILED ((void *) -1) // same as MAP_FAILED

size_t MAP_THRESHOLD = 128 * 1024;

// head of the memory list allocated
static struct block_meta *head;
static int heap_preallocated;

//------------------ HELPER MEMORY MANAGEMENT FUNCTION -----------------//

// add memory block in list:
// - heap blocks are added at the end of the list
// - mmap blocks are added at the start of the list
static void add_memory_block(struct block_meta *block)
{
	if (block->status == STATUS_MAPPED) {
		if (head == NULL) {
			head = block;
		} else {
			block->next = head;
			head = block;
		}
	} else if (block->status == STATUS_ALLOC) {
		if (head == NULL) {
			head = block;
		} else {
			struct block_meta *current = head;
			struct block_meta *prev = NULL;

			while (current) {
				prev = current;
				current = current->next;
			}
			DIE(prev == NULL, strcat("Error adding heap block(STATUS_ALLOC) in:", __func__));
			prev->next = block;
		}
	} else if (block->status == STATUS_FREE && heap_preallocated == 0) {
		if (head == NULL) {
			head = block;
		} else {
			struct block_meta *current = head;
			struct block_meta *prev = NULL;

			while (current) {
				prev = current;
				current = current->next;
			}
			DIE(prev == NULL, strcat("Error adding heap block(STATUS_FREE) in:", __func__));
			prev->next = block;
		}
	} else {
		DIE(1 == 1, strcat("Error trying to add mmap block in:", __func__));
	}
}

// convert (void *) to (struct block_meta *)
static struct block_meta *get_block_ptr(void *ptr)
{
	return (struct block_meta *)(((struct block_meta *) ptr) - 1);
}

// convert (struct block_meta *) to (void *)
void *get_ptr_block(struct block_meta *block)
{
	return (void *)(block + 1);
}

// remove memory block form list
static void remove_memory_block(struct block_meta *block)
{
	struct block_meta *current = head;
	struct block_meta *prev = NULL;

	while (current) {
		if (current == block) {
			if (current == head)
				head = head->next;
			else
				prev->next = block->next;
			block->next = NULL;
			break;
		}
		prev = current;
		current = current->next;
	}
}

// check if a block is in list
int is_block_in_memory(struct block_meta *block)
{
	struct block_meta *current = head;

	while (current) {
		if (current == block)
			return 1;
		current = current->next;
	}
	return 0;
}

// request memory space on heap if size + BLOCK_META_SIZE < MAP_THRESHOLD
// or else allocate the pointer with mmap
static struct block_meta *request_memory(size_t size)
{
	struct block_meta *block = NULL;
	size_t total_size = ALIGN(size) + ALIGN(BLOCK_META_SIZE);

	if (total_size < MAP_THRESHOLD)
		block = (struct block_meta *) sbrk(total_size);
	else
		block = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (block == ALLOCATION_FAILED || block == NULL)
		return NULL; // allocation failed.

	block->size = ALIGN(size);
	block->next = NULL;

	if (total_size < MAP_THRESHOLD)
		block->status = STATUS_ALLOC;
	else
		block->status = STATUS_MAPPED;
	return block;
}

// preallocate a heap of 128kb with header size included
static void preallocate_heap(void)
{
	struct block_meta *heap = (struct block_meta *) sbrk(PREALLOC_SIZE);

	DIE(heap == NULL, strcat("Error heap preallocation in:", __func__));
	heap->size = PREALLOC_SIZE - ALIGN(BLOCK_META_SIZE);
	heap->next = NULL;
	heap->status = STATUS_FREE;
	add_memory_block(heap);
}

// coalesce 2 blocks (it will be called for adjenct blocks)
static void coalesce_blocks(struct block_meta *block1, struct block_meta *block2)
{
	block1->size += block2->size + ALIGN(BLOCK_META_SIZE);
	block1->next = block2->next;
	block1->status = STATUS_FREE;
}

// coalesces adjenct memory blocks
static void coalesce_memory(void)
{
	struct block_meta *current = head;

	while (current && current->next) {
		if (current->status == STATUS_FREE && current->next->status == STATUS_FREE)
			coalesce_blocks(current, current->next);
		current = current->next;
	}
}

// attempt to split the block if possible for better memory management
// if a split can't be made, mark the memory as being allocated and used
// size = block_meta + payload + padding (alignment of 8 bytes)
static void split_block(struct block_meta *block, size_t size)
{
	if (block->size >= ALIGN(size) + ALIGN(BLOCK_META_SIZE) + ALIGNMENT) {
		struct block_meta *new_block = (struct block_meta *)((char *)block + ALIGN(BLOCK_META_SIZE) + ALIGN(size));

		new_block->size = block->size - ALIGN(size) - ALIGN(BLOCK_META_SIZE);
		new_block->next = block->next;
		new_block->status = STATUS_FREE;

		block->size = ALIGN(size);
		block->next = new_block;
		block->status = STATUS_ALLOC;
	} else {
		block->status = STATUS_ALLOC;
	}
}

// return the best fitting block in memory if exists, otherwise NULL
static struct block_meta *find_best_free_block(struct block_meta **last, size_t size)
{
	struct block_meta *current = head;
	struct block_meta *best = NULL;

	// coalesce memory before search
	coalesce_memory();
	while (current) {
		if (current->status == STATUS_FREE && current->size >= size) {
			if (best == NULL)
				best = current;
			else if (best->size > current->size)
				best = current;
		}
		*last = current;
		current = current->next;
	}

	return best;
}

// find last block in list and check if is a FREE block
static struct block_meta *find_last_free_block(void)
{
	struct block_meta *prev = NULL;
	struct block_meta *current = head;

	while (current) {
		prev = current;
		current = current->next;
	}
	if (prev->status == STATUS_FREE)
		return prev;
	return NULL;
}

// min on size_t type
size_t min(size_t a, size_t b)
{
	return (a < b) ? a : b;
}

//----------------------------------------------------------------------//

void *os_malloc(size_t size)
{
	/* TODO: Implement os_malloc */
	if (size == 0)
		return NULL;

	size_t total_size = ALIGN(size) + ALIGN(BLOCK_META_SIZE);
	struct block_meta *block = NULL;

	if (total_size < MAP_THRESHOLD) {
		if (!heap_preallocated) {
			preallocate_heap();
			heap_preallocated = 1;
		}

		struct block_meta *last = head;

		block = find_best_free_block(&last, ALIGN(size));

		if (block) {
			// found a memory block on the prealloacated heap
			// try to split the block, otherwise the function will mark the zone as in use
			split_block(block, size);
		} else {
			// try to expand the last free block
			struct block_meta *last = find_last_free_block();

			if (last == NULL) {
				// there is no free block to expand
				// request additional memory and add it in the list of memory management
				block = request_memory(size);
				DIE(block == NULL, strcat("Error request new heap block in:", __func__));
				add_memory_block(block);
			} else {
				// found a block to expand at the end of the heap
				block = request_memory(size - last->size - ALIGN(BLOCK_META_SIZE));
				DIE(block == NULL, strcat("Error expanding heap block in:", __func__));
				block->status = STATUS_FREE;
				coalesce_blocks(last, block);
				block = last;
				block->status = STATUS_ALLOC;
			}
		}
	} else {
		// request additional memory and add it in the list of memory management
		block = request_memory(size);
		DIE(block == NULL, strcat("Error request new mmap block in:", __func__));
		add_memory_block(block);
	}

	return get_ptr_block(block);
}

void os_free(void *ptr)
{
	/* TODO: Implement os_free */
	if (ptr == NULL)
		return;

	struct block_meta *block = get_block_ptr(ptr);

	if (!is_block_in_memory(block))
		return;
	if (block->status == STATUS_ALLOC) {
		block->status = STATUS_FREE;
		coalesce_memory();
	} else if (block->status == STATUS_MAPPED) {
		remove_memory_block(block);

		size_t len = block->size + BLOCK_META_SIZE;
		int ret = munmap((void *) block, len);

		DIE(ret != 0, strcat("Error munmap in:", __func__));
	}
}

void *os_calloc(size_t nmemb, size_t size)
{
	MAP_THRESHOLD = getpagesize();
	void *ptr = os_malloc(nmemb * size);

	MAP_THRESHOLD = 128 * 1024;

	if (ptr == NULL)
		return NULL;
	memset(ptr, 0, nmemb * size);
	return ptr;
}

void *os_realloc(void *ptr, size_t size)
{
	/* TODO: Implement os_realloc */
	if (ptr == NULL)
		return os_malloc(size);

	if (size == 0) {
		os_free(ptr);
		return NULL;
	}

	struct block_meta *block = get_block_ptr(ptr);
	void *new_ptr = NULL;

	if (block->status == STATUS_FREE)
		return NULL;

	size = ALIGN(size);
	// do nothing if size didn't change
	if (block->size == size)
		return ptr;

	// on heap realloc
	if (block->status == STATUS_ALLOC) {
		if (block->size > size) {
			// truncate the block and try to split it
			split_block(block, size);
			new_ptr = get_ptr_block(block);
		} else {
			// try to expand the block
			coalesce_memory();
			if (block->next && block->next->status == STATUS_FREE &&
				block->size + block->next->size + ALIGN(BLOCK_META_SIZE) >= size) {
				// expand the zone
				block->size += block->next->size + ALIGN(BLOCK_META_SIZE);
				block->next = block->next->next;
				block->status = STATUS_ALLOC;
				// split the zone after to future reuse
				split_block(block, size);
				new_ptr = get_ptr_block(block);
			} else if (block->next == NULL) {
				// the block on the heap is the last one so we need just to
				// expand with additional size
				os_free(get_ptr_block(block));
				new_ptr = os_malloc(size);
				if (new_ptr == NULL)
					return NULL;
				memmove(new_ptr, get_ptr_block(block), min(block->size, size));
			} else {
				// could not expand the block
				// remove it from memory, coalesce memory and try to find a new place for it
				new_ptr = os_malloc(size);
				if (new_ptr == NULL)
					return NULL;
				memmove(new_ptr, get_ptr_block(block), min(block->size, size));
				os_free(get_ptr_block(block));
			}
		}
	} else if (block->status == STATUS_MAPPED) {
		new_ptr = os_malloc(size);
		if (new_ptr == NULL)
			return NULL;
		memmove(new_ptr, get_ptr_block(block), min(block->size, size));
		os_free(get_ptr_block(block));
	}

	return new_ptr;
}
