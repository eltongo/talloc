#ifndef __TALLOC_H__
#define __TALLOC_H__

#define TALLOC_MAGIC 0xab91ea94 // magic for integrity checking
#define TALLOC_ALLOC_PAGES 1000 // how many pages to allocate per arena

#include <unistd.h>
#include <sys/mman.h>

// This struct represents a free chunk of memory
// It's basically a node in a linked list of chunks
typedef struct __talloc_chunk_t {
	size_t size; // available size in the chunk
	struct __talloc_chunk_t *next; // next free chunk
} talloc_chunk_t;

// This struct represents the header for an allocated
// region of memory. This header is stored just before
// the allocated memory we return a pointer to on allocation.
typedef struct __talloc_header_t {
	size_t size; // size of the allocated memory
	int magic; // the magic field which should be equal to TALLOC_MAGIC
} talloc_header_t;

// This struct represents an arena. These are basically larger "chunks"
// of memory, holding multiple smaller chunks of memory (depending on requests).
// Total allocated space for the arena is stored in `allocated`. However, this
// includes the space needed for this struct, as well as the space taken by
// chunk headers (talloc_chunk_t).
// This is a linked list node, specifically a doubly linked list node, since
// it has a pointer to the previous element.
typedef struct __talloc_arena_t {
	size_t allocated; // total space taken by the arena including space needed for metadata
	size_t max_free_space; // space of the largest free chunk available
	talloc_chunk_t *free_list; // free chunks linked list
	struct __talloc_arena_t *next; // next arena in the list
	struct __talloc_arena_t *prev; // previous arena in the list
} talloc_arena_t;

// the size of reserved space for a newly allocated arena
#define TALLOC_ARENA_OVERHEAD (sizeof(talloc_arena_t) + sizeof(talloc_chunk_t))

// This struct represents the state of our allocator.
typedef struct __talloc_state_t {
	talloc_arena_t *arena_head; // the head of the arena linked list
	talloc_arena_t *arena_tail; // the tail of the arena linked list
	size_t minallocsize, pagesize; // the page size
	char initialized; // has the first arena been allocated?
} talloc_state_t;

// our state is stored here
talloc_state_t state;

// Initializes an allocated arena.
void TAlloc_init_arena(talloc_arena_t *arena, size_t allocated) {
	arena->allocated = allocated;
	arena->max_free_space = allocated - TALLOC_ARENA_OVERHEAD;
	arena->next = NULL;
	arena->prev = NULL;
	// the free chunks linked list starts right after the arena header/struct
	talloc_chunk_t *free_list = (talloc_chunk_t *) (arena + 1);
	free_list->size = arena->max_free_space;
	free_list->next = NULL;
	arena->free_list = free_list;
}

// Initialize the allocator's state, and allocate the first arena.
void TAlloc_initialize() {
	state.pagesize = getpagesize();
	state.minallocsize = state.pagesize * TALLOC_ALLOC_PAGES;
	state.arena_head = mmap(NULL, state.minallocsize, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
	if (state.arena_head == MAP_FAILED) {
		state.arena_head = NULL;
		return;
	}
	state.arena_tail = state.arena_head;
	TAlloc_init_arena(state.arena_head, state.minallocsize);
	state.initialized = 1;
}

// Allocate memory for a new arena. The resulting arena will
// be at least state.minallocsize, no matter how small the 
// space needed is. If it's greater than state.minallocsize,
// then the allocated size will be a multiple of state.pagesize.
talloc_arena_t * TAlloc_create_arena(size_t space_needed) {
	// account for possible overflow
	if (space_needed + TALLOC_ARENA_OVERHEAD < space_needed) return NULL;
	space_needed += TALLOC_ARENA_OVERHEAD;

	size_t to_allocate;

	if (space_needed < state.minallocsize) {
		// ensure we allocate at least state.minallocsize bytes
		to_allocate = state.minallocsize;
	} else if (space_needed > state.minallocsize) {
		// check if not evenly divided by page size
		// we always map multiples of page size
		unsigned int add_one = space_needed % state.pagesize > 0;
		to_allocate = state.pagesize * ((space_needed / state.pagesize) + add_one);
	}

	
	void *new_arena = mmap(NULL, to_allocate, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
	if (new_arena == MAP_FAILED) {
		return NULL;
	}

	talloc_arena_t *arena = (talloc_arena_t *) new_arena;
	// initialize the newly created arena
	TAlloc_init_arena(arena, to_allocate);

	return arena;
}

// Called when we can't find enough free space in existing arenas.
// This will call TAlloc_create_arena to create a new arena and return it
talloc_arena_t * TAlloc_alloc_more_space(size_t space_needed) {
	talloc_arena_t *arena = TAlloc_create_arena(space_needed);
	if (!arena) {
		return NULL;
	}

	// insert the newly created arena into the linked list
	state.arena_tail->next = arena;
	arena->prev = state.arena_tail;
	state.arena_tail = arena;

	return arena;
}

// Frees an arena. This is called when an arena (not the first one) is
// no longer needed. We simply remove it from the linked list, and unmap it.
void TAlloc_free_arena(talloc_arena_t *arena) {
	talloc_arena_t *prev = arena->prev;
	talloc_arena_t *next = arena->next;

	if (!munmap(arena, arena->allocated)) {
		prev->next = next;
		if (next) next->prev = prev;
	}
}

// When a chunk is freed/updated, we want to merge it with any adjacent
// empty chunks, so that we have a larger free chunk vs two or more smaller free chunks
void TAlloc_coalesce(talloc_chunk_t *chunk) {
	// ensure the next free chunk starts right after the current chunk before
	// coalescing/merging them.
	if (chunk->next == (void *) chunk + chunk->size + sizeof(talloc_chunk_t)) {
		chunk->size += sizeof(talloc_chunk_t) + chunk->next->size;
		chunk->next = chunk->next->next;
	}
}

// Adjust the max free space of the arena. If a recently freed/updated chunk has more
// free space than the current "max free space", then we update the arena accordingly.
void TAlloc_adjust_space_for_new_chunk(talloc_arena_t *arena, talloc_chunk_t *chunk) {
	if (chunk->size > arena->max_free_space) {
		arena->max_free_space = chunk->size;
	}
}

// Check if a given pointer is inside an arena.
int TAlloc_ptr_in_arena(talloc_arena_t *arena, void *ptr) {
	return ptr >= (void *) (arena + 1) && ptr < (void *) arena + arena->allocated;
}

// Find the arena that contains a given pointer
talloc_arena_t * TAlloc_find_arena(void *ptr) {
	talloc_arena_t *arena = state.arena_head;
	while (arena && !TAlloc_ptr_in_arena(arena, ptr)) arena = arena->next;
	return arena;
}

// Free the allocated memory at the given pointer. This will do some basic
// integrity checking, such as ensuring the pointer points to a location within
// an arena, and that the header's magic holds the correct value.
// Finally it will coalesce any adjacent free chunks.
void TAlloc_free(void *ptr) {
	if (!state.initialized) return;
	talloc_arena_t *arena = TAlloc_find_arena(ptr);
	if (!arena) return;

	talloc_header_t *header = (talloc_header_t *) ptr - 1;
	if (header->magic != TALLOC_MAGIC) {
		return;
	}

	talloc_chunk_t *chunk = (talloc_chunk_t *) header;

	// chunks are sorted based on their address to make coalescing easier
	if (!arena->free_list) {
		arena->free_list = chunk;
		arena->free_list->next = NULL;
		arena->max_free_space = chunk->size;
	} else if (chunk < arena->free_list) {
		chunk->next = arena->free_list;
		arena->free_list = chunk;
		TAlloc_coalesce(chunk);
		TAlloc_adjust_space_for_new_chunk(arena, chunk);
	} else {
		talloc_chunk_t *insert_after = arena->free_list;
		while (insert_after->next && insert_after->next < chunk) {
			insert_after = insert_after->next;
		}
		chunk->next = insert_after->next;
		insert_after->next = chunk;
		TAlloc_coalesce(chunk);
		TAlloc_adjust_space_for_new_chunk(arena, chunk);
		TAlloc_coalesce(insert_after);
		TAlloc_adjust_space_for_new_chunk(arena, insert_after);
	}

	// unless it's the first arena, we release the occupied space if no longer needed
	if (arena != state.arena_head && arena->allocated == arena->max_free_space + TALLOC_ARENA_OVERHEAD) {
		TAlloc_free_arena(arena);
	}
}

// Find an arena that contains a free chunk big enough to accommodate
// the given size.
talloc_arena_t * TAlloc_get_accommodating_arena(size_t size) {
	talloc_arena_t *arena_node = state.arena_head;
	while (arena_node && arena_node->max_free_space < size) arena_node = arena_node->next;
	if (!arena_node) {
		// existing arenas don't have enough free space; time to create a new one
		arena_node = TAlloc_alloc_more_space(size);
	}

	return arena_node;
}

// Our "malloc" replacement. This is what clients will call to
// allocate memory.
//
// This function will essentially
//  - initialize the allocator state if necessary
//  - find an arena that has a chunk big enough to accommodate the given size
//    (or fail if not possible)
//  - split the chunk of memory if it's bigger than necessary
//  - update the free list of the arena
//  - return the pointer to the allocated memory to the caller
//
// There are some other details in here, such as coalescing the created free
// chunk when we split the one we found, and updating max_free_space of the
// arena accordingly.
void * TAlloc_malloc(size_t size) {
	if (!state.initialized) TAlloc_initialize();
	if (size == 0) return NULL;
	// find the arena that contains a chunk that can accommodate this size
	talloc_arena_t *arena = TAlloc_get_accommodating_arena(size);

	// oops; cannot allocate any more space :(
	if (!arena) return NULL;

	talloc_chunk_t *head = arena->free_list;
	talloc_chunk_t *prev = NULL;
	while (head && head->size < size) {
		prev = head;
		head = head->next;
	}

	if (!head) return NULL;

	talloc_chunk_t *next_free_chunk;

	size_t excess_space = head->size - size;
	size_t allocated_space = size;
	char max_free_space_affected = head->size >= arena->max_free_space;

	if (excess_space > sizeof(talloc_chunk_t)) {
		next_free_chunk = (talloc_chunk_t *) ((void *) head + sizeof(talloc_chunk_t) + size);
		// excess space needs to be greater than the size of the chunk header
		// otherwise we will "take the loss"
		next_free_chunk->size = excess_space - sizeof(talloc_chunk_t);
		next_free_chunk->next = head->next;
		TAlloc_coalesce(next_free_chunk);
		TAlloc_adjust_space_for_new_chunk(arena, next_free_chunk);
		// this new chunk can potentially be bigger than current "max free space", so
		// if we can avoid some calculations, why not do that?
		max_free_space_affected = max_free_space_affected && head->size > next_free_chunk->size;
	} else {
		next_free_chunk = head->next;
		allocated_space += excess_space;
	}

	// initialize the header of the allocated chunk of memory
	talloc_header_t *alloc_header = (talloc_header_t *) head;
	alloc_header->magic = TALLOC_MAGIC;
	alloc_header->size = allocated_space;

	if (!prev) arena->free_list = next_free_chunk;
	else arena->free_list->next = next_free_chunk;

	if (max_free_space_affected) {
		if (!arena->free_list) {
			arena->max_free_space = 0;
		} else {
			talloc_chunk_t *chunk = arena->free_list;
			arena->max_free_space = chunk->size;
			while ((chunk = chunk->next) != NULL) {
				if (chunk->size > arena->max_free_space) {
					arena->max_free_space = chunk->size;
				}
			}
		}
	}

	// note that the pointer points to the location
	// right after the header :)
	return (void *) (alloc_header + 1);
}

// A helper function that prints what the heap looks like
// at a certain point in time.
void TAlloc_debug_print() {
	if (!state.initialized) {
		printf("TAlloc is not yet initialized\n");
		return;
	}
	talloc_arena_t *arena = state.arena_head;
	while (arena) {
		printf("Arena at %p, %lu bytes, %lu reserved\n",
			arena, arena->allocated, sizeof(talloc_arena_t));
		void *ptr = (void *) (arena + 1);
		while (ptr < (void *) arena + arena->allocated) {
			talloc_header_t *header = (talloc_header_t *) ptr;
			if (header->magic == TALLOC_MAGIC) {
				printf("  Allocated chunk at %p, %lu bytes, %lu reserved\n",
					header, header->size, sizeof(talloc_header_t));
				ptr += sizeof(talloc_header_t) + header->size;
			} else {
				talloc_chunk_t *chunk = (talloc_chunk_t *) ptr;
				printf("  Free chunk at %p, %lu bytes, %lu reserved\n",
					chunk, chunk->size, sizeof(talloc_chunk_t));
				ptr += sizeof(talloc_chunk_t) + chunk->size;
			}
		}
		arena = arena->next;
	}
}

#endif
