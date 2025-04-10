# disproving_nonatomic_petrson
my textbook for OS studies shows Peterson's_algorithm without stating that it cant work without atomic reads+writes we are gona show why those are needed

note that petersons algorithem does work for systems without complex caching, or systems that run on a single thread thats pinned to a core, this would not be an issue and petersons algorithem would just work.

the issue here is entirly todo with how cache lines work. we need to get into a situation where a cache line is stale and that can only happen if there was a WRITE operation to it that did not write to the cache a READ operation is reading from. this can only happen if there is more than 1 cache line involved.