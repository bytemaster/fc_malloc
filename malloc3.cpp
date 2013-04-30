/**
 *   Each thread has its own 'arena' where it can allocate 'new' blocks of what ever size it needs (buckets). After
 *   a thread is done with memory it places it in a garbage collection queue.
 *
 *   The garbage collector follows each threads trash bin and moves the blocks into a recycled list that
 *   all other threads can pull from.
 *
 *   The garbage collector can grow these queues as necessary and shrink them as time progresses.
 */

#include <vector>
#include <unordered_set>

#include "mmap_alloc.hpp"
#include "disruptor.hpp"
#include <thread>
#include <stdint.h>
#include <memory.h>
#include <stdlib.h>
#include <iostream>
#include <vector>
#include <assert.h>
#include <unistd.h>
//#include "rand.cpp"


using namespace disruptor;

#define PAGE_SIZE (4*1024*1024)
#define BENCH_SIZE ( (1024) )
#define ROUNDS 2
#define LOG2(X) ((unsigned) (8*sizeof (unsigned long long) - __builtin_clzll((X)) - 1))
#define NUM_BINS 32 // log2(PAGE_SIZE)

class block_header
{
   public:
      block_header* next()
      { 
         if( _size > 0 ) return reinterpret_cast<block_header*>(data()+_size); 
         else return nullptr;
      }
      block_header* prev()
      { 
         if( _prev_size <= 0 ) return nullptr;
         return reinterpret_cast<block_header*>(reinterpret_cast<char*>(this) - _prev_size - 8);
      }

      char*         data()      { return ((char*)this)+8; }
      size_t        size()const { return abs(_size); }

      /** create a new block at p and return it */
      block_header* split_after( size_t s )
      {
         
     //    printf( "split after %d  _size %d   size() - 8 - s: %d\n", (int)s, _size, int(size()-8-s) );
         if(  (size() - 8 -32) < s ) return nullptr;// no point in splitting to less than 32 bytes
       //  if( size()-8-s < 32 ) return nullptr; 

         block_header* n = reinterpret_cast<block_header*>(data()+s);
         n->_prev_size   = s;
         n->_size        = size() -s -8;
         if( _size < 0 ) n->_size = -n->_size; // we just split the tail
         _size = s; // this node now has size s
         assert( size() >= s );
         return n;
      }

      // merge this block with next, return head of new block.
      block_header* merge_next()
      {
         auto nxt = next();
         if( !nxt ) return this;
         assert( (char*)nxt - (char*)this == _size + 8 );
         _size += 8 + nxt->size();
         auto nxt_nxt = nxt->next();
         if( nxt_nxt ) 
         {
             nxt_nxt->_prev_size += 8 + nxt->size();
         }
         else
         {
            _size = -_size; // we are now the end.
         }
         return this;
      }

      // merge this block with the prev, return the head of new block
      block_header* merge_prev() 
      {
         auto pre = prev();
         if( !pre ) return this;
         assert( (char*)this - (char*)pre == _prev_size + 8 );
         auto _s = size() + 8;
         assert( pre->_size > 0 );
         pre->_size += _s;

         auto nxt = next();
         if( nxt ) nxt->_prev_size += _s;
         else pre->_size = -pre->_size; // 

         return pre;
      }

//   private:
      int32_t   _prev_size; // size of previous header.
      int32_t   _size; // offset to next, negitive indicates tail
};

/** returns a new block page allocated via mmap 
 *  The page has 2 block headers (head+tail) defined
 *  and head is returned.
 **/
block_header* allocate_block_page();

struct block_list_node
{
    block_list_node():next(nullptr){};
    block_list_node* next;

    block_header&    header()
    {
      return  *reinterpret_cast<block_header*>(reinterpret_cast<char*>(this)-8);
    }

    block_list_node* find_end() 
    {
       block_list_node* n = this;
       while( n->next )
       {
          n = n->next;
       }
       return n;
    }
};


class thread_allocator
{
  public:
    char*   alloc( size_t s );

    void    free( char* c )
    {
        block_header* b = reinterpret_cast<block_header*>(c) - 1;
  //      printf( "free bh %p d %p  _size  %d  _prev_size %d\n", b, c, b->_size, b->_prev_size );

        if( cache(b) ) return;

        auto node = reinterpret_cast<block_list_node*>(c); // store a point
        node->next = _gc_on_deck.next;
        if( !_gc_at_bat.next )
        {
           _gc_at_bat.next = node;
           _gc_on_deck.next = nullptr;
        }
        else
        {
           _gc_on_deck.next = node;
        }
    }

    static thread_allocator& get()
    {
        static __thread thread_allocator* tld = nullptr;
        if( !tld )  // new is not an option
        { 
            tld = reinterpret_cast<thread_allocator*>( mmap_alloc( sizeof(thread_allocator) ) );
            tld = new (tld) thread_allocator(); // inplace construction

            // TODO: allocate  pthread_threadlocal var, attach a destructor /clean up callback
            //       to that variable... 
        }
        return *tld;
    }

  protected:
    /**
     *  dynamic cache size based on usage makes
     *  this a 'tricky' method.  Attempt to cache
     *  'h' and return true if successful.  Successful
     *  caching reduces contention on the
     *  atomic add.
     */
    bool  cache( block_header* h ) 
    { 
       auto b = LOG2(h->size()); 
       if( _bin_cache_size[b] < 4 )
       {
           printf( "cache %d in bin %d  size: %d\n", (int)h->size(), (int)b, int(1<<b) );
           assert( h->size() >= 1<<b );
           reinterpret_cast<block_list_node*>(h->data())->next = _bin_cache[b].next;
           _bin_cache[b].next = reinterpret_cast<block_list_node*>(h->data());
           _bin_cache_size[b]++;
           return true; 
       } return false;
    }



    block_header* fetch_block_from_bin( int bin );

    thread_allocator();
    ~thread_allocator();

    friend class garbage_collector;
    bool            _done;       // cleanup and remove from list.
    block_list_node _gc_at_bat;  // where the gc pulls from.
    uint64_t        _gc_pad[7];  // gc thread and this thread should not false-share these values
    block_list_node _gc_on_deck; // where we save frees while waiting on gc to bat.

    /** 
     * called by gc thread and pops the at-bat free list
     */
    block_list_node*  get_garbage() // grab a pointer previously claimed.
    {
      if( block_list_node* gar = _gc_at_bat.next )
      {
         _gc_at_bat.next = nullptr;
         return gar;
      }
      return nullptr;
    }
    block_list_node             _bin_cache[NUM_BINS];      // head of cache for specific bin
    uint16_t                    _bin_cache_size[NUM_BINS]; // track num of nodes in cache

    thread_allocator*           _next; // used by gc to link thread_allocs together
};


typedef thread_allocator* thread_alloc_ptr;


/**
 *   Polls all threads for freed items.
 *   Upon receiving a freed item, it will look
 *   at its size and move it to the proper recycle
 *   bin for other threads to consume.
 *
 *   When there is less work to do, the garbage collector
 *   will attempt to combine blocks into larger blocks
 *   and move them to larger cache sizes until it
 *   ultimately 'completes a page' and returns it to
 *   the system.  
 *
 *   From the perspective of the 'system' an alloc
 *   involves a single atomic fetch_add.
 *
 *   A free involves a non-atomic store.
 *
 *   No other sync is necessary.
 */
class garbage_collector
{
  public:
    garbage_collector();
    ~garbage_collector();

    class recycle_bin
    {
       public:
          recycle_bin()
          :_read_pos(0),_full_count(0),_full(2),_write_pos(0)
          {
             memset( &_free_queue, 0, sizeof(_free_queue) );
          }

          // read the _read_pos without any atomic sync, we only care about an estimate
          int64_t available()                            { return _write_pos - *((int64_t*)&_read_pos); }
          // reserve right to read the next num spots from buffer
          int64_t claim( int64_t num )                   { return _read_pos.fetch_add(num, std::memory_order_relaxed); }
          block_header* get_block( int64_t claim_pos )   { return _free_queue.at(claim_pos); }
          void          clear_block( int64_t claim_pos ) { _free_queue.at(claim_pos) = nullptr; }

          // determines how many chunks should be required to consider this bin full.
          // TODO: this method needs to be tweaked to factor in 'time'... as it stands
          // now the GC loop will be very agressive at shrinking the queue size
          int64_t       check_status()
          {
              auto av = available();
              if( av <= 0 )
              {
                 // apparently there is high demand, the consumers cleaned us out.
                 _full *= 2; // exponential growth..
                 _full = std::min( _full+4, _free_queue.get_buffer_size() -1 );
              }
              else if( av == _full )
              {
                 // apparently no one wanted any... we should shrink what we consider full
                 _full -= 4; // fast back off
                 if( _full < 2 ) _full = 2;
              }
              else // av < _full
              {
                 // some, but not all have been consumed... 
                 // if less than half have been consumed... reduce size,
                 // else keep the size the same.
                 if( av > _full/2 )
                 {
                     _full--; // reduce full size,slow back off
                     if( _full < 2 ) _full = 2;
                     return  _full - av; 
                 }
                 else // more than half consumed... keep full size the same, refill
                 {
                 }
              }
              return _full - av; 
          }



          ring_buffer<block_header*,1024*256>   _free_queue; 
          std::atomic<int64_t>                  _read_pos; //written to by read threads
          int64_t _pad[7];     // below this point is written to by gc thread
          int64_t _full_count; // how many times gc thread checked and found the queue full
          int64_t _full;       // limit the number of blocks kept in queue
          int64_t _write_pos;  // read by consumers to know the last valid entry.

          // used to find blocks available for merging.
          std::unordered_set<block_header*>     _free_set; 
    };

    recycle_bin& find_bin_for( block_header* h ) 
    { 
      return get_bin(get_bin_num( h->size() )); 
    }

    int get_bin_num( size_t s )
    {
      return LOG2(s);
    }

    recycle_bin&  get_bin( size_t bin_num ) 
    { 
        assert( bin_num < NUM_BINS );
        return _bins[bin_num];
    }

    void register_allocator( thread_alloc_ptr ta );

    static garbage_collector& get()
    {
        static garbage_collector gc;
        return gc;
    }
  private:
    static void  run();
    // threads that we are actively looping on
    std::atomic<thread_alloc_ptr> _thread_head;

    std::thread                _thread; // gc thread.. doing the hard work
    recycle_bin                _bins[NUM_BINS];


    static std::atomic<bool>   _done;
};
std::atomic<bool> garbage_collector::_done(false);

garbage_collector::garbage_collector()
:_thread_head(nullptr),_thread( &garbage_collector::run )
{
}
garbage_collector::~garbage_collector()
{
  _done.store(true, std::memory_order_release );
  _thread.join();
}

void garbage_collector::register_allocator( thread_alloc_ptr ta )
{
  printf( "registering thread allocator %p\n", ta );

  auto* stale_head = _thread_head.load(std::memory_order_relaxed);
  do { ta->_next = stale_head;
  }while( !_thread_head.compare_exchange_weak( stale_head, ta, std::memory_order_release ) );
}

void  garbage_collector::run()
{
    garbage_collector& self = garbage_collector::get();
    while( true )
    {
        thread_alloc_ptr cur_al = *((thread_alloc_ptr*)&self._thread_head);
        bool found_work = false;

        // for each thread, grab all of the free chunks and move them into
        // the proper free set bin, but save the list for a follow-up merge
        // that takes into consideration all free chunks.
        while( cur_al )
        {
          auto cur = cur_al->get_garbage();

          if( cur ) found_work = true; 

          while( cur )
          {
              block_header* c = &cur->header();
              block_header* n = c->next();
              block_header* p = c->prev();

           //   printf( "freeing block with size: %d  %d   prev_size: %d\n", (int)c->size(), c->_size, c->_prev_size );
              
              // check to see if the next block is free
            #if 1
              if( n )
              {
                recycle_bin& n_bin = self.find_bin_for(n);
                auto itr = n_bin._free_set.find(n);
                if( itr != n_bin._free_set.end() )
                {
               //     printf( "merging block with next._prev_size = %d  next._size = %d   _size = %d\n", 
                //                n->_prev_size, n->_size, c->_size );
                    n_bin._free_set.erase(itr);
                
                    // it is free, merge it with c
                    c = c->merge_next();
                }
              }
              
              if( p ) 
              {
                  // check to see if the next block is free
                  recycle_bin& p_bin = self.find_bin_for(p);
                  auto pitr = p_bin._free_set.find(p);
                  if( pitr != p_bin._free_set.end() )
                  {
              //        printf( "merging block with prev._prev_size = %d  prev._size = %d   _prev_size = %d  delta ptr %d %p %p\n", 
               //                 p->_prev_size, p->_size, c->_prev_size,  int((char*)c - (char*)p), c, p);
                      p_bin._free_set.erase(pitr);
                      // it is free, merge it with c
                      c = c->merge_prev();
                  }
              }
            #endif

              // store the potentially combined block 
              // in the proper bin.
              recycle_bin& c_bin = self.find_bin_for(c);
              c_bin._free_set.insert(c);

              // get the next free chunk
              assert( cur != cur->next );
              cur = cur->next;
          }
          // get the next thread.
          assert( cur_al != cur_al->_next );
          cur_al = cur_al->_next;
        }

        // for each recycle bin, check the queue to see if it
        // is getting low and if so, put some chunks in play
        for( int i = 0; i < NUM_BINS; ++i )
        {
            garbage_collector::recycle_bin& bin = self._bins[i];
            auto needed = bin.check_status(); // returns the number of chunks need
            if( needed > 0 )
            {
              found_work = true;
              int64_t next_write_pos = bin._write_pos;
              for( auto bitr = bin._free_set.begin(); 
                        needed && bitr != bin._free_set.end();
                         )
              {
                 ++next_write_pos;
                 if( nullptr != bin._free_queue.at(next_write_pos) )
                 {
                    // apparently a reader skipped one due to contention, it
                    // can be reclaimed now..
                 }
                 else 
                 {
                     bin._free_queue.at(next_write_pos) = *bitr;
                     bitr =  bin._free_set.erase(bitr);
                 }
                 --needed;
              }
              bin._write_pos = next_write_pos;
            }
            else if( needed < 0 )
            {
              // apparently no one is checking this size class anymore, we can reclaim some nodes.
              // TODO:  perhaps we only do this if there is no other work found as work implies
              // that the user is still allocating / freeing objects and thus we don't want to
              // compete to start freeing cache yet... 
            }
        }

        if( _done.load( std::memory_order_acquire ) ) return;
        if( !found_work ) 
        {
            // reclaim cache
            // sort... 


            usleep( 1000 );
        }
    }
}


block_header* allocate_block_page()
{
    fprintf( stderr, "\n\n                                                                               ALLOCATING NEW PAGE\n\n" );
    auto limit = mmap_alloc( PAGE_SIZE );

    block_header* bl = reinterpret_cast<block_header*>(limit);
    bl->_prev_size = 0;
    bl->_size = - (PAGE_SIZE-8);
    return bl;
}

thread_allocator::thread_allocator()
{
  _done            = false;
  _next            = nullptr;
  _gc_at_bat.next  = nullptr;
  _gc_on_deck.next = nullptr;
  garbage_collector::get().register_allocator(this);
  memset( _bin_cache, 0, sizeof(_bin_cache) );
  memset( _bin_cache_size, 0, sizeof(_bin_cache_size) );
}

thread_allocator::~thread_allocator()
{
  // give the rest of our allocated chunks to the gc thread
  // free all cache, free _alloc_block
  _done = true;
}

int get_min_bin( size_t s )
{
  return LOG2(s)+1;
}

char* thread_allocator::alloc( size_t s )
{
//    printf( "alloc %d\n", (int)s );
    if( s == 0 ) return nullptr;
    // calculate min block size
    s = 32*((s + 31)/32); // multiples of 32 bytes

    // if greater than PAGE_SIZE mmap_alloc
    if( s > (PAGE_SIZE-8) )
    {
       auto limit = mmap_alloc( PAGE_SIZE );
       block_header* bl = reinterpret_cast<block_header*>(limit);
       bl->_prev_size = -s; // PAGE ALLOCATED blocks have a negitive prev.
       bl->_size = 0; // no forward size... 'this is the end' if this block
                      // somehow gets mixed in with others.
       assert( bl->size() >= s );
       return bl->data();
    }
    
    for( int bin = get_min_bin(s); bin < NUM_BINS; ++bin )
    {
        block_header* b = fetch_block_from_bin(bin);
        if( b )
        {
        printf( "    USING BIN bin: %d (%d) for chunk size %d\n", bin, int(1<<bin), (int)s );
          // printf( "1) b->size: %d  s: %d\n", (int)b->size(), (int)s );
           block_header* tail = b->split_after( s );
         //  printf( "2) b->size: %d  s: %d\n", (int)b->size(), (int)s );
           assert( b->size() >= s );
           if( tail ) 
              this->free( tail->data() );
        //   printf( "3) b->size: %d  s: %d   delta: %d\n", (int)b->size(), (int)s, int(b->size() - s) );
           assert( b->size() >= s );
           return b->data();
        }
    }

    block_header* new_page = allocate_block_page();
    //printf( "      alloc new block page   %p  _size  %d _prev_size %d  next %p  prev %p\n",
      //    new_page, new_page->_size, new_page->_prev_size, new_page->next(), new_page->prev() );
    block_header* tail = new_page->split_after(s);
//    printf( "      alloc free tail  %p  _size  %d _prev_size %d  next %p  prev %p  tail %p\n",
 //         tail, tail->_size, tail->_prev_size, tail->next(), tail->prev(), tail );
    
    if( tail )
    {
  //    printf( "        FREE TAIL SIZE: %d   %p  %p\n", (int)tail->size(), tail, tail->data() );
      this->free( tail->data() );
    }
    assert( new_page->size() >= s );
    return new_page->data();
}

/**
 *  Checks our local bin first, then checks the global bin.
 *
 *  @return null if no block found in cache.
 */
block_header* thread_allocator::fetch_block_from_bin( int bin )
{
    if( _bin_cache[bin].next ) 
    {
        _bin_cache_size[bin]--;
        block_header* bh = &_bin_cache[bin].next->header();
        _bin_cache[bin].next = reinterpret_cast<block_list_node*>(bh->data())->next;
        return bh;
    }
    else
    {
        garbage_collector& gc              = garbage_collector::get();
        garbage_collector::recycle_bin& rb = gc.get_bin( bin );

        if( auto avail = rb.available()  )
        {
            // claim up to half of the available, just incase 2
            // threads try to claim at once, they both can, but
            // don't hold a cache of more than 4 items
            auto claim_num = std::min<int64_t>( avail/2, 3 ); 
            // claim_num could now be 0 to 3
            claim_num++; // claim at least 1 and at most 4

            // this is our one and only atomic 'sync' operation... 
            auto claim_pos = rb.claim( claim_num );
            auto claim_end = claim_pos + claim_num;
            bool found = false;
            while( claim_pos != claim_end )
            {
               block_header* h = rb.get_block(claim_pos);
               if( h )
               {
                  found = true;
                  rb.clear_block(claim_pos); // let gc know we took it. 
                  auto ln = reinterpret_cast<block_list_node*>(h->data());
                  ln->next = _bin_cache[bin].next;
                  _bin_cache[bin].next = ln;
               }
               else // oops... I guess 3 tried to claim at once...
               {
                  // drop it on the floor and let the
                  // gc thread pick it up next time through the
                  // ring buffer.
               }
               ++claim_pos;
            }
            if( found ) 
               return fetch_block_from_bin(bin); // grab it from the cache this time.
        }
    }
    return nullptr;
}

char* malloc2( int s )
{
  return thread_allocator::get().alloc(s);
}

void  free2( char* s )
{
  return thread_allocator::get().free(s);
}


/*  SEQUENTIAL BENCH
int main( int argc, char** argv )
{
  if( argc == 2 && argv[1][0] == 'S' )
  {
     printf( "malloc2\n");
     for( int i = 0; i < 50000000; ++i )
     {
        char* test = malloc2( 128 );
        assert( test != nullptr );
        test[0] = 1;
        free2( test );
     }
  }
  if( argc == 2 && argv[1][0] == 's' )
  {
     printf( "malloc\n");
     for( int i = 0; i < 50000000; ++i )
     {
        char* test = (char*)malloc( 128 );
        assert( test != nullptr );
        test[0] = 1;
        free( test );
     }
  }
  fprintf( stderr, "done\n");
 // sleep(5);
  return 0;
}
*/

/* RANDOM BENCH */
std::vector<char*>  buffers[16];
void pc_bench_worker( int pro, int con, char* (*do_alloc)(int s), void (*do_free)(char*)  )
{
  for( int r = 0; r < ROUNDS; ++r )
  {
      for( size_t x = 0; x < buffers[pro].size()/2 ; ++x )
      {
         uint32_t p = rand() % buffers[pro].size();
         if( !buffers[pro][p] )
         {
           uint64_t si = 32 + rand()%(8096*16); //4000;//32 + rand() % (1<<16);
           auto r = do_alloc( si );
           assert( r != nullptr );
         //  assert( r[0] != 99 ); 
         //  r[0] = 99; 
           buffers[pro][p] = r;
         }
      }
      for( size_t x = 0; x < buffers[con].size()/2 ; ++x )
      {
         uint32_t p = rand() % buffers[con].size();
         assert( p < buffers[con].size() );
         assert( con < 16 );
         assert( con >= 0 );
         if( buffers[con][p] ) 
         { 
           //assert( buffers[con][p][0] == 99 ); 
          // buffers[con][p][0] = 0; 
           do_free(buffers[con][p]);
           buffers[con][p] = 0;
         }
      }
  }
}


void pc_bench(int n, char* (*do_alloc)(int s), void (*do_free)(char*)  )
{
  for( int i = 0; i < 16; ++i )
  {
    buffers[i].resize( BENCH_SIZE );
    memset( buffers[i].data(), 0, 8 * BENCH_SIZE );
  }

  std::thread* a = nullptr;
  std::thread* b = nullptr;
  std::thread* c = nullptr;
  std::thread* d = nullptr;
  std::thread* e = nullptr;
  std::thread* f = nullptr;
  std::thread* g = nullptr;
  std::thread* h = nullptr;
  std::thread* i = nullptr;
  std::thread* j = nullptr;


 int s = 1;
  switch( n )
  {
     case 10:
     a = new std::thread( [=](){ pc_bench_worker( n, s, do_alloc, do_free ); } );
     n--;
     s++;
     case 9:
      b = new std::thread( [=](){ pc_bench_worker( n, s, do_alloc, do_free ); } );
     n--;
     s++;
     case 8:
      c = new std::thread( [=](){ pc_bench_worker( n, s, do_alloc, do_free ); } );
     n--;
     s++;
     case 7:
      d = new std::thread( [=](){ pc_bench_worker( n, s, do_alloc, do_free ); } );
     n--;
     s++;
     case 6:
     e = new std::thread( [=](){ pc_bench_worker( n, s, do_alloc, do_free ); } );
     n--;
     s++;
     case 5:
     f = new std::thread( [=](){ pc_bench_worker( n, s, do_alloc, do_free ); } );
     n--;
     s++;
     case 4:
      g = new std::thread( [=](){ pc_bench_worker( n, s, do_alloc, do_free ); } );
     n--;
     s++;
     case 3:
      h = new std::thread( [=](){ pc_bench_worker( n, s, do_alloc, do_free ); } );
     n--;
     s++;
     case 2:
      i = new std::thread( [=](){ pc_bench_worker( n, s, do_alloc, do_free ); } );
     n--;
     s++;
     case 1:
      j = new std::thread( [=](){ pc_bench_worker( n, s, do_alloc, do_free ); } );
  }
  if(a)
  a->join();
  if(b)
  b->join();
  if(c)
  c->join();
  if(d)
  d->join();
  if(e)
  e->join();
  if(f)
  f->join();
  if(g)
  g->join();
  if(h)
  h->join();
  if(i)
  i->join();
  if(j)
  j->join();

}
void pc_bench_st(char* (*do_alloc)(int s), void (*do_free)(char*)  )
{
  for( int i = 0; i < 16; ++i )
  {
    buffers[i].resize( BENCH_SIZE );
    memset( buffers[i].data(), 0, 8 * BENCH_SIZE );
  }
  //int i = 0;
  std::thread a( [=](){ pc_bench_worker( 1, 1, do_alloc, do_free ); } );
  a.join();
}
//#include <tbb/scalable_allocator.h>

char* do_malloc(int s)
{ 
    return (char*)::malloc(s); 
//   return (char*)scalable_malloc(s);
}
void  do_malloc_free(char* c)
{ 
//    scalable_free(c);
   ::free(c); 
}

int main( int argc, char** argv )
{
  if( argc > 2 && argv[1][0] == 'm' )
  {
    std::cerr<<"malloc multi\n";
    pc_bench( atoi(argv[2]), do_malloc, do_malloc_free );
    return 0;
  }
  if( argc > 2 && argv[1][0] == 'M' )
  {
    std::cerr<<"hash malloc multi\n";
    pc_bench( atoi(argv[2]), malloc2, free2 );
    return 0;
  }
  if( argc > 1 && argv[1][0] == 's' )
  {
    std::cerr<<"malloc single\n";
    pc_bench_st( do_malloc, do_malloc_free );
    return 0;
  }
  if( argc > 1 && argv[1][0] == 'S' )
  {
    std::cerr<<"hash malloc single\n";
    pc_bench_st( malloc2, free2 );
    return 0;
  }

  printf( "alloc\n" );
  char* tmp = malloc2( 61 );
  usleep( 1000 );
  char* tmp2 = malloc2( 134 );
  usleep( 1000 );
  char* tmp4 = malloc2( 899 );
  printf( "a %p  b %p   c %p\n", tmp, tmp2, tmp4 );

  usleep( 1000 );

  printf( "free\n" );
  free2( tmp );
  usleep( 1000 );
  free2( tmp2 );
  usleep( 1000 );
  free2( tmp4 );

  usleep( 1000*1000 );

  printf( "alloc again\n" );
  char* tmp1 = malloc2( 61 );
  usleep( 1000 );
  char* tmp3 = malloc2( 134 );
  usleep( 1000 );
  char* tmp5 = malloc2( 899 );
  printf( "a %p  b %p   c %p\n", tmp1, tmp3, tmp5 );
  free2( tmp1 );
  free2( tmp3 );
  free2( tmp4 );

  usleep( 1000*1000 );

  return 0;
}







