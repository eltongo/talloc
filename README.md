# TAlloc
Disclaimer: this is a non-production-ready proof of concept memory allocator, which may or may not be suitable for real world programs (hint: it's not, but don't let it hear you, or you might hurt its feelings).

I've been reading the well-known [Operating Systems: Three Easy Pieces](https://pages.cs.wisc.edu/~remzi/OSTEP/) book, and I got a little inspired to write a proof-of-concept memory allocator for fun<sup>1</sup> and intellectual profit.

## Why did you write this?
As I said before, I only wrote this for fun. It's not mean to be fast, let alone efficient. It's supposed to work, but alas I haven't tested it extensively. It was a nice learning experience for me, and I hope it might be helpful for someone out there. If not, well, I had my fun :)

## How does it work? (in a nutshell)

Some approaches use `brk` and `sbrk` to implement an allocator. I admit I was close to using the same calls, but eventually decided to use `mmap` instead. But I digress. The exact details probably merit a blog post, so I'll try to keep it short.

Normally when you call `TAlloc_malloc()` (our `malloc()` replacement), we have to acquire that amount of memory from the OS somehow – using `mmap` in this case. And always asking the OS for small bits of extra memory would be inefficient and wasteful. So, instead, we acquire memory from the OS in larger chunks (which I have called `arena`s), and then manage those arenas ourselves. When a request comes to allocate some bytes of memory, a slice of memory is taken from the arena, instead, to fulfil the request.

Depending on the number and size of allocation requests, we might end up with multiple arenas, each containing its own chunks of memory.

This memory allocator uses a "first fit" approach. That is, in a list of free chunks, it chooses the first chunk that has a memory greater than or equal to the requested amount of memory. If the chunk is greater, it gets split into two, and one of them is returned to the user.

## Any shortcomings I should be aware of?

Besides the fact that this is not meant to be used in the real world? I've only used this on my M1 Mac; it should work on Linux, but I haven't tested it.

## How do I use this?

As they say, a piece of code is worth a thousand words<sup>2</sup>:

```c
#include <stdio.h>
#include "talloc.h"

typedef struct __int_array_t {
    size_t length;
    int *array;
} int_array_t;

int main(int argc, char **argv) {
    int_array_t *arr = (int_array_t *) TAlloc_malloc(sizeof(int_array_t));
    arr->array = (int *) TAlloc_malloc(sizeof(int) * 10);
    arr->length = 10;
    for (int i = 0; i < arr->length; ++i) {
        arr->array[i] = i;
    }
    for (int i = arr->length - 1; i >= 0; --i) {
        printf("%d ", arr->array[i]);
    }
    printf("\n");
    
    TAlloc_free(arr->array);
    TAlloc_free(arr);
}
```

The example is slightly convoluted, but should not be very hard to follow.

There are two functions you need to know:
 - `TAlloc_malloc(size_t)` - which allocated memory of a given size
 - `TAlloc_free(void *)` - which frees the given pointer

There's also another function, which is useful if you want to see what the memory layout looks like. The function is `TAlloc_debug_print()`. As the name suggests, this function will print the layout of the memory at a certain point in time. Here's how to use it:

```c
#include <stdio.h>
#include "talloc.h"

int main(int argc, char **argv) {
  // some allocations to make things happen
  char *lol = (char *) TAlloc_malloc(10 * sizeof(char));
  char *lmao = (char *) TAlloc_malloc(50 * sizeof(char));
  char *wow = (char *) TAlloc_malloc(2000 * sizeof(char));
  int *cool = (int *) TAlloc_malloc(5000000 * sizeof(int));
  
  // and here's the starring function
  TAlloc_debug_print();

  return 0;
}
```

And this is what it outputs on my machine:

```txt
Arena at 0x10083c000, 16384000 bytes, 40 reserved
  Allocated chunk at 0x10083c028, 10 bytes, 16 reserved
  Allocated chunk at 0x10083c042, 50 bytes, 16 reserved
  Allocated chunk at 0x10083c084, 2000 bytes, 16 reserved
  Free chunk at 0x10083c864, 16381836 bytes, 16 reserved
Arena at 0x1017dc000, 20004864 bytes, 40 reserved
  Allocated chunk at 0x1017dc028, 20000000 bytes, 16 reserved
  Free chunk at 0x102aeed38, 4792 bytes, 16 reserved
```

As you can see, in my machine this code has created two arenas. For each arena, the function will output the allocations and the free chunks, their addresses, and their sizes. For arenas, the number of bytes includes reserved space, whereas for chunks, the number of bytes does not include the reserved space.

Now, normally the allocator does not maintain any reference to allocated chunks. However, this function makes an educated guess about the type of chunk (i.e. allocated or free) based on the value where the magic should be. This should work fine, but I wrote this function in a hurry, so you never know...

It might be a good idea to try different variations of allocating and freeing memory, and then calling `TAlloc_debug_print()` to peek under the hood.

## Do you have any benchmarks or performance stats?

No. Feel free to produce your own if you're into that sort of thing. Efficiency and performance are very important for a real world memory allocator, but this one has no such ambitions. He wants to stay a wooden boy<sup>3</sup> for life!

---
<sup>1</sup> If you can call torturing yourself fun!  
<sup>2</sup> Okay, nobody says that.  
<sup>3</sup> I hadn't made up my mind on whether this was a she, a he or an it. This settles it I guess.
