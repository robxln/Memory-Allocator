# Memory Management Strategy

In this project, I've implemented a memory management system using simple linked lists. The approach involves retaining two types of allocated memory blocks: those allocated with `mmap` and those allocated on the heap with `sbrk`. Here's how it works:

## Memory Allocation with `os-malloc`

The `os-malloc` function follows these strategies:

1. If the requested memory size is smaller than a threshold (`MAP_THRESHOLD`), I allocate memory on the heap:
   - On the first call, I preallocate the heap space.
   - I attempt to find the best-fit block (closest in size) using the `find_best` method. Once found, I split it, keeping the information in the left block (i.e., the same pointer).
   - If a suitable block isn't found on the heap, I try expanding the last block on the heap, provided it's marked as FREE.
   - If none of the above works, I allocate a new memory area of the specified size using `mmap`.

2. If the requested memory size is greater than or equal to `MAP_THRESHOLD`, I allocate the block directly using `mmap` and add it to the beginning of the list.

## Memory Deallocation with `os-free`

The `os-free` function handles memory deallocation. If a pointer is invalid (NULL or hasn't been previously allocated), no action is taken. Here's how it works:

1. Blocks allocated with `sbrk` on the heap are marked as FREE.
2. Blocks allocated with `mmap` are released using `munmap`, and the respective block is removed from the list.

## Memory Allocation with `os-calloc`

I first modify the `MAP_THRESHOLD` to `page_size`, but this change is temporary and is reset to 128KB later. Then, I call `os-malloc`, and upon success, I initialize the entire block to 0 using `memset`.

## Memory Reallocation with `os-realloc`

I handle two special cases at the beginning:

- If the requested `size` is 0, I call `os-free` on the provided pointer.
- If the pointer `ptr` is NULL, I call `malloc` with the specified `size`.

For reallocating memory, I consider the following scenarios:

### If `ptr` is on the heap

1. If the new `size` is smaller, I truncate it using the `split_block` method.
2. If the new `size` is larger, I try the following strategies:
   - Attempt to expand the block if there is a sufficiently large free block immediately after it.
   - If there are no available blocks after it, I allocate the difference in size to extend the block. For better results, I mark the current block as FREE before calling `os_malloc`. This allows for coalescing and the possibility of finding an already allocated area on the heap, even with overlaps.
   - If none of the strategies work, I allocate a new block of the desired size with `os-malloc`, move the information using `memmove`, and finally, call `os-free` on the old pointer.

### If `ptr` is allocated with `mmap`

I allocate a new block of the desired size with `os-malloc`, move the information using `memmove`, and then call `os-free` on the old pointer.
