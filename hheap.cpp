#include <atomic>
#include <stdint.h>
#include <memory.h>
#include <stdlib.h>
#include <iostream>
#include <vector>
#include <assert.h>
#include <unistd.h>
#include <mutex>
#include <thread>
std::mutex print_mutex;
#include "disruptor.hpp"

using namespace disruptor;

#if 0
#define PRINT( ... )  \
{ std::unique_lock<std::mutex> _lock(print_mutex); \
    __VA_ARGS__ \
}
#define NEW_PRINT( ... ) \
{ std::unique_lock<std::mutex> _lock(print_mutex); \
  __VA_ARGS__ \
}
#define PAGE_FREE_PRINT( ... ) \
{ std::unique_lock<std::mutex> _lock(print_mutex); \
  __VA_ARGS__ \
}
#else
  #define PRINT(...)
  #define NEW_PRINT(...)
  #define PAGE_FREE_PRINT(...)
#endif



int64_t fast_rand();

struct slot_header
{ 
    int32_t page_id;     // used by free to find the page in the pool
    int16_t pool_id;     // used by free to find the pool
    uint8_t page_slot;   // the slot in the page in the pool
    uint8_t alignment;   // 8 if reserved, 0 if free... byte _data[alignment-1] = alignment.
};



template<uint32_t Size, uint32_t NumSlots>
struct page
{
  public:
    struct slot 
    { 
        int32_t page_id;     // used by free to find the page in the pool
        int16_t pool_id;     // used by free to find the pool
        uint8_t page_slot;   // the slot in the page in the pool
        uint8_t alignment;   // 8 if reserved, 0 if free... byte _data[alignment-1] = alignment.
        char    _data[Size]; // alignment helps us find the page_id/pool_id when allocated aligned objects.
    };

    page(int16_t page_id, int16_t pool_id)
    :_free_write_cursor(NumSlots)
    {
       _pool_id = pool_id;
       _page_id = page_id;
       _posted  = false;

     // ... 
       _free_write_cursor.follows( _free_read_cursor );
       _free_read_cursor.follows( _free_write_cursor );

       for( int i = 0; i < NumSlots; ++i )
       {
          slot& s = _slot[i];
          s.page_id = page_id;
          s.pool_id = pool_id;
          s.page_slot = i;
          s.alignment = 8; // free expects this 
          this->free(i); // increment the free write cursor
       }
       _release_free_pos = 0;

       assert( free_estimate() == NumSlots );
       assert( can_alloc() );
    }

    int32_t free_estimate() 
    { 
       if( _release_free_pos < 0 ) return 0;
       return _free_write_cursor.begin() - _release_free_pos;
    }

    bool can_alloc()
    {
       if( _free_read_cursor.begin() == _free_read_cursor.end() &&
           _free_read_cursor.begin() == _free_read_cursor.check_end() )
       { 
    //      std::cerr<<"    CAN ALLOC? page: "<<_page_id<<" free read cursor begin: "<<_free_read_cursor.begin()<<"   end: "<<_free_read_cursor.end()<<"\n";
          return false; 
       }
       return true;
    }

    char*  alloc(uint8_t align = 8)
    {
       if( !can_alloc() ) return nullptr; 
      
       auto    pos       = _free_read_cursor.begin();
       int64_t free_slot = _free_list.at(pos);
       _free_read_cursor.publish( pos );
        
     //  std::cerr<<"page: "<<_page_id<<" alloc slot: "<<int(free_slot)<<"  alignment: "<<int(align)<<"  free list pos: "<<pos<<"\n";
       assert( free_slot < NumSlots);
       assert( _slot[free_slot].alignment == 0 ); // make the spot as used and take its alignment

       //assert( _slot[free_slot].alignment == 0); // make the spot as used and take its alignment
       _slot[free_slot].alignment = align; // make the spot as used and take its alignment
       return _slot[free_slot]._data;
     //  uint8_t* rtn = (uint8_t*)_slot[free_slot]._data + align - 8; // TODO: adjust for alignment..
     //  rtn[-1] = align;
     //  return (char*)rtn;
    } 
    
    /** return the number of slots freed since this page was 'released' */
    uint64_t    free( uint8_t slot )
    {
    //   std::cerr<<"free slot: "<<int(slot)<<"  alignment: "<<int(_slot[slot].alignment)<<"\n";

       assert( slot < NumSlots );
       assert( _slot[slot].alignment >= 8 );
       assert( _slot[slot].pool_id == _pool_id );
       
       _slot[slot].alignment = 0; // last thing we do is set alignment.

       auto cl = _free_write_cursor.claim(1);
       _free_list.at(cl) = slot;
       //_free_write_cursor.publish_after( cl, cl - 1 );
       _free_write_cursor.publish( cl );//, cl - 1 );

       return free_estimate();
       return 0;
    }

    /** called to save the free cursor position so we can track how many
     *  slots have been freed since this thread gave up control 
     */
    void  release()
    {
        _posted = false;
        _free_claim.store(0,std::memory_order_relaxed);
        _release_free_pos = _free_write_cursor.begin();
    }

    void  claim()
    {
        _release_free_pos = -1;
    }
    bool  claim_free()
    {
       if( !_posted && 0 == _free_claim.fetch_add(1, std::memory_order_release ) )
       {
         return  _posted = true;
       }
       return false;
    }
    bool  is_posted_to_free_list(){ return _posted; }

   private:
    slot                                         _slot[NumSlots]; // actual data storage

    /** the position of the free_write_cursor at the time this page was 'released' 
     *  by the last allocator thread.
     **/
    int64_t                                      _release_free_pos;

    ring_buffer<uint16_t,2*NumSlots>             _free_list;
    shared_write_cursor                          _free_write_cursor;
    read_cursor                                  _free_read_cursor;
    uint32_t                                     _pool_id;
    uint32_t                                     _page_id;
    bool                                         _posted; 
    std::atomic<int>                             _free_claim;
};

/**
 *    A pool is a collection of 'pages' that threads can claim to use
 *    for allocation.  
 *
*/
template<uint16_t PoolId, uint32_t Size,uint32_t SlotsPerPage,uint32_t MaxPages=1024*32>
struct pool
{
   typedef page<Size,SlotsPerPage>  page_type;
   typedef page_type*               page_ptr;
   typedef typename page_type::slot slot_type;
   typedef slot_type*               slot_ptr;

   struct thread_local_data 
   {
      thread_local_data()
      :current_page_num(-1),
       current_page(nullptr){}

      int32_t    current_page_num;
      page_ptr   current_page;
   };

   ring_buffer<uint32_t,MaxPages>   _free_pages; // indexes into _alloc_pages
   shared_write_cursor              _free_page_write_cursor;
   shared_read_cursor               _free_page_read_cursor;

   ring_buffer<page_ptr,MaxPages>   _alloc_pages; // pages allocated (fixed index)
   shared_write_cursor              _page_alloc_cursor;
   const read_cursor                _page_alloc_begin; // used to prevent alloc_cursor from wrapping

   pool()
   :_free_page_write_cursor( MaxPages ),
    _free_page_read_cursor( MaxPages ),
    _page_alloc_cursor( MaxPages )
   {
      _free_page_write_cursor.follows( _free_page_read_cursor );
      _free_page_read_cursor.follows( _free_page_write_cursor );
     // _page_alloc_cursor.follows( _page_alloc_begin );
      //_page_alloc_begin.follows( _page_alloc_cursor ); // begin shouldn't move
   }

   static pool& instance() 
   { 
      static pool _p;
      return _p;
   }

   static thread_local_data*& local_pool()
   {
      static thread_local thread_local_data*  _current = nullptr;
      return _current;
   }

   thread_local_data&  get_local_pool()
   {
      thread_local_data*& cur = local_pool();
      if( cur == nullptr )
      {
         cur = new thread_local_data();
      }
      return *cur;
   }

   char* do_alloc( uint16_t align = 8 )
   {
       thread_local_data& tld = get_local_pool(); //get thread local data

       if( tld.current_page_num == -1 )  // we need to claim a page
       {
          claim_page(tld);
          assert( tld.current_page_num != -1 );
          assert( tld.current_page );
       }
       char* c = tld.current_page->alloc(align);

       while( !c )  // no space available, claim a new page
       { 
         claim_page(tld); 
         c = tld.current_page->alloc(align);
         if( !c ) 
         {
          std::cerr<<"!!?? NULL??\n";
         }
       }
       return c;
   }

   void do_free( char* c )
   {
      uint8_t* s = reinterpret_cast<uint8_t*>(c);
      assert( c != nullptr );
      assert( s[-1] == 8 ); 
      uint8_t* slot_pos = (uint8_t*)c-8;//s + s[-1]-16; // s-1 == alignment, default 8 byte

      slot_ptr sl = reinterpret_cast<slot_ptr>(slot_pos);
      assert( sl->pool_id == PoolId        ); 
      assert( sl->page_slot < SlotsPerPage );
      assert( sl->page_id < MaxPages       );
     
      auto p = _alloc_pages.at(sl->page_id);
      if( p->free(sl->page_slot) > SlotsPerPage/4 )
      {
          if( !p->claim_free() ) return; // do I get to post this.. or does someone else..
          // move page into free queue
          auto claim = _free_page_write_cursor.claim(1);
          _free_pages.at(claim) = sl->page_id;


          PAGE_FREE_PRINT(std::cerr<<"PAGE AVAILABLE: "<<sl->page_id<<"\n";
          std::cerr<<"    sl->pool_id: "<<int(sl->pool_id)<<"  slot: "<<int(sl->page_slot)<<"  id: "<<int(sl->page_id)<<" SlotsPerPage: "<<SlotsPerPage<<"   available_slots: "<<p->free_estimate()<<" \n";
          std::cerr<<"    free_page_write claim: "<<claim<<"\n";
          )

          _free_page_write_cursor.publish_after( claim, claim -1 );
      }
   }

   void claim_page( thread_local_data& tld )
   {
       if( tld.current_page ) tld.current_page->release(); 

       auto read_claim =  _free_page_read_cursor.claim(1);
       if(  !_free_page_read_cursor.is_available( read_claim ) )
       { 
          NEW_PRINT(std::cerr<<"NEW PAGE:    free_read_claim_idx: "<<read_claim<<"\n";)
          auto free_write_idx = _free_page_write_cursor.claim(1); // claim a place to store the 'free' allocated page
          NEW_PRINT(std::cerr<<"             free_write_idx: "<<free_write_idx<<"\n";)

       // the read position is after the next write position... allocate
          // allocate and publish page_idx ... to both free page cursors
          auto alloc_idx  = _page_alloc_cursor.claim(1); // claim a place to allocate.. 
          NEW_PRINT(std::cerr<<"             alloc_write_idx: "<<alloc_idx<<" READ  "<<read_claim<<"\n";)

          _alloc_pages.at(alloc_idx) = new page_type( alloc_idx, PoolId ); // TODO: replace with mmap
          _page_alloc_cursor.publish_after( alloc_idx, alloc_idx-1 ); // publish the allocated buffer
          NEW_PRINT(std::cerr<<"                 alloc published: "<<alloc_idx<<"  READ "<<read_claim<<" \n";)

          _free_pages.at(free_write_idx) = alloc_idx;           
          //_free_page_write_cursor.publish_after(free_write_idx,free_write_idx-1); // publish the new 'free' buffer
          _free_page_write_cursor.publish(free_write_idx);//,free_write_idx-1); // publish the new 'free' buffer
          NEW_PRINT(std::cerr<<"                 free write idx published: "<<free_write_idx<<"  value: "<<_free_pages.at(free_write_idx)<<"\n";)

          NEW_PRINT( std::cerr<<"                READ CLAIM: "<<read_claim<<"\n";);
         // _free_page_read_cursor.wait_for( read_claim );
          auto ridx = _free_pages.at(read_claim);
          NEW_PRINT( std::cerr<<"                 free_page read publish: "<<read_claim<<"  value: "<<ridx<<"\n";)
          //_free_page_read_cursor.publish_after(read_claim,read_claim-1);
          _free_page_read_cursor.publish(read_claim);//,read_claim-1);

          tld.current_page_num = ridx;
          tld.current_page     = _alloc_pages.at(tld.current_page_num);
       }
       else
       {
        NEW_PRINT( std::cerr<<"RECLAIM PAGE:  free_read_claim_idx: "<<read_claim<<"  page: "<<_free_pages.at(read_claim)<<"\n";)
          tld.current_page_num = _free_pages.at(read_claim);
          //_free_page_read_cursor.publish_after(read_claim,read_claim-1);
          _free_page_read_cursor.publish( read_claim );
          tld.current_page     = _alloc_pages.at(tld.current_page_num);
        NEW_PRINT( std::cerr<<"               published free_read_claim_idx: "<<read_claim<<"\n"; )
        NEW_PRINT( std::cerr<<"               available: "<< tld.current_page->free_estimate()<<"\n"; )
       }
       tld.current_page->claim();
   }
   
   static void   free( char* c )             { instance().do_free(c);             };
   static char*  alloc( uint16_t align = 8 ) { return instance().do_alloc(align); };
};


#define BENCH_SIZE ( (1024*256) )
#define ROUNDS 100 
//#define BENCH_SIZE ( (512) )
//#define ROUNDS 5 



#include <thread>
void malloc_bench( int tid )
{
  std::vector<char*> a(BENCH_SIZE);
  memset( a.data(), 0, a.size() * sizeof(char*));
  for( int x = 0; x < ROUNDS; ++x )
  {
    for( int i = 0; i < BENCH_SIZE; ++i )
    {
      int pos = rand() & 1;
      if( a[i] && pos )
      {
          free(a[i]); 
          a[i]=0;
      }
      else if( !a[i] && pos )
      {
          a[i] = (char*)malloc(64);
      }
    }
  }
}
void bench(int tid)
{
  std::vector<char*> a(BENCH_SIZE);
  memset( a.data(), 0, a.size() * sizeof(char*));
  for( int x = 0; x < ROUNDS; ++x )
  {
    for( int i = 0; i < BENCH_SIZE; ++i )
    {
      int pos = rand() & 1;
      if( a[i] && pos )
      {
          pool<1,64,256>::free(a[i]); 
          a[i] = 0;//free(a[i]); 
      }
      else if( !a[i] && pos )
      {
         a[i] = pool<1,64,256>::alloc();
      }
    }
  }
}

std::vector<char*>  buffers[16];


void pc_bench_worker( int pro, int con, char* (*do_alloc)(int s), void (*do_free)(char*)  )
{
  for( int r = 0; r < ROUNDS; ++r )
  {
      for( int x = 0; x < buffers[pro].size()/2 ; ++x )
      {
         int p = fast_rand() % buffers[pro].size();
         if( !buffers[pro][p] )
         {
           auto si = 60; //fast_rand() % (1<<15);
           auto r = do_alloc( si );

           slot_header* sh = (slot_header*)(r-8);// TODO: handle alignment
           //assert( sh->alignment == 8 );
           //assert( sh->pool_id > 3 );

           if( r == nullptr )
           {
            std::cerr<<"size: "<<si<<"  returned null\n";
           }
           assert( r != nullptr );
           assert( r[0] != 99 ); 
           r[0] = 99; 
           buffers[pro][p] = r;
         }
      }
      for( int x = 0; x < buffers[con].size()/2 ; ++x )
      {
         int p = fast_rand() % buffers[con].size();
         if( buffers[con][p] ) 
         { 
           //assert( buffers[con][p][0] == 99 ); 
           buffers[con][p][0] = 0; 
           do_free(buffers[con][p]);
           buffers[con][p] = 0;
         }
      }
  }
}
#if 0
void pc_bench_worker( int pro, int con, char* (*do_alloc)(), void (*do_free)(char*)  )
{
  for( int r = 0; r < ROUNDS; ++r )
  {
     // produce some
     for( int i = 0; i < buffers[pro].size(); ++i )
     {
        // don't wrap...
      //  while( buffers[pro][i] ) usleep(0);
        buffers[pro][i] = do_alloc();
     }
     for( int i = 0; i < BENCH_SIZE*2; ++i )
     {
        rand() % buffers[con].size()
     }

     usleep( 100 );
     for( int i = 0; i < buffers[pro].size(); ++i )
     {
     //   while( !buffers[con][i] ) usleep(0);
        if( buffers[con][i] )
        {
           do_free(buffers[con][i]);
           buffers[con][i] = 0;
        }
     }
  }
}
#endif

void pc_bench(char* (*do_alloc)(int s), void (*do_free)(char*)  )
{
  for( int i = 0; i < 16; ++i )
  {
    buffers[i].resize( BENCH_SIZE );
    memset( buffers[i].data(), 0, 8 * BENCH_SIZE );
  }
  std::thread a( [=](){ pc_bench_worker( 1, 2, do_alloc, do_free ); } );
  std::thread b( [=](){ pc_bench_worker( 2, 3, do_alloc, do_free ); } );
  std::thread c( [=](){ pc_bench_worker( 3, 4, do_alloc, do_free ); } );
  std::thread d( [=](){ pc_bench_worker( 4, 5, do_alloc, do_free ); } );
  std::thread e( [=](){ pc_bench_worker( 5, 6, do_alloc, do_free ); } );
  std::thread f( [=](){ pc_bench_worker( 6, 7, do_alloc, do_free ); } );
  std::thread g( [=](){ pc_bench_worker( 7, 8, do_alloc, do_free ); } );
  std::thread h( [=](){ pc_bench_worker( 8, 9, do_alloc, do_free ); } );
  std::thread i( [=](){ pc_bench_worker( 9, 10, do_alloc, do_free ); } );
  std::thread j( [=](){ pc_bench_worker( 10, 1, do_alloc, do_free ); } );

  a.join();
  b.join();
  c.join();
  d.join();
  e.join();
  f.join();
  g.join();
  h.join();
  i.join();
  j.join();
}
void pc_bench_st(char* (*do_alloc)(int s), void (*do_free)(char*)  )
{
  for( int i = 0; i < 16; ++i )
  {
    buffers[i].resize( BENCH_SIZE );
    memset( buffers[i].data(), 0, 8 * BENCH_SIZE );
  }
  int i = 0;
  std::thread a( [=](){ pc_bench_worker( 1, 1, do_alloc, do_free ); } );
  /*
  std::thread b( [=](){ pc_bench_worker( 2, 2, do_alloc, do_free ); } );
  std::thread c( [=](){ pc_bench_worker( 3, 3, do_alloc, do_free ); } );
  std::thread d( [=](){ pc_bench_worker( 4, 4, do_alloc, do_free ); } );
  std::thread e( [=](){ pc_bench_worker( 5, 5, do_alloc, do_free ); } );
  std::thread f( [=](){ pc_bench_worker( 6, 6, do_alloc, do_free ); } );
  std::thread g( [=](){ pc_bench_worker( 7, 7, do_alloc, do_free ); } );
  std::thread h( [=](){ pc_bench_worker( 8, 8, do_alloc, do_free ); } );
  */

  a.join();
  /*
  b.join();
  c.join();
  d.join();
  e.join();
  f.join();
  g.join();
  h.join();
  */
}

char* do_malloc(int s){ return (char*)malloc(s); }
void  do_malloc_free(char* c){ free(c); }
char* do_hash_malloc(int s)
{ 
    #define LOG2(X) ((unsigned) (8*sizeof (unsigned long long) - __builtin_clzll((X)) - 1))
    switch( LOG2(s)+1 )
    {
       case 64:
            assert("!dont malloc yet.." );
            return (char*)malloc(s);
       case 16:
            return pool<16,1<<16,8>::alloc(); 
       case 15:
            return pool<15,1<<15,16>::alloc(); 
       case 14:
            return pool<14,1<<14,32>::alloc(); 
       case 13:
            return pool<13,1<<13,64>::alloc(); 
       case 12:
            return pool<12,1<<12,64>::alloc(); 
       case 11:
            return pool<11,1<<11,64>::alloc(); 
       case 10:
            return pool<10,1<<10,128>::alloc(); 
       case 9:
            return pool<9,1<<9,128>::alloc(); 
       case 8:
            return pool<8,1<<8,128>::alloc(); 
       case 7:
            return pool<7,1<<7,256>::alloc(); 
       case 6:
            return pool<6,1<<6,256>::alloc(); 
       case 5:
       default:
            return pool<5,1<<5,256>::alloc(); 
    }
    assert( !"we shoudln't get here!" );
}


void  do_hash_free(char* c)
{ 
    assert( c != nullptr );
    uint8_t a = *(c-1); // alignment
    slot_header* sh = (slot_header*)(c-8);// TODO: handle alignment
    assert( a == 8 );
    if( !(sh->pool_id >=5 && sh->pool_id <= 16 ) )
    {
      PRINT( std::cerr<< "ERROR: pool_id: "<<sh->pool_id<<"\n"; 
          std::cerr.flush();
          assert( sh->pool_id >=5 && sh->pool_id <= 16 );
      );
    }
    switch( sh->pool_id )
    {
       case 16:
            pool<16,1<<16,8>::free(c); 
            return;
       case 15:
            pool<15,1<<15,16>::free(c); 
            return;
       case 14:
            pool<14,1<<14,32>::free(c); 
            return;
       case 13:
            pool<13,1<<13,64>::free(c); 
            return;
       case 12:
            pool<12,1<<12,64>::free(c); 
            return;
       case 11:
            pool<11,1<<11,64>::free(c); 
            return;
       case 10:
            pool<10,1<<10,128>::free(c); 
            return;
       case 9:
            pool<9,1<<9,128>::free(c); 
            return;
       case 8:
            pool<8,1<<8,128>::free(c); 
            return;
       case 7:
            pool<7,1<<7,256>::free(c); 
            return;
       case 6:
            pool<6,1<<6,256>::free(c); 
            return;
       case 5:
       default:
            pool<5,1<<5,256>::free(c); 
            return;
    }
    assert( !"we shoudln't get here!" );
}


int main( int argc, char** argv )
{
  if( argc > 1 && argv[1][0] == 'm' )
  {
    std::cerr<<"malloc multi\n";
    pc_bench( do_malloc, do_malloc_free );
  }
  if( argc > 1 && argv[1][0] == 'M' )
  {
    std::cerr<<"hash malloc multi\n";
    pc_bench( do_hash_malloc, do_hash_free );
  }
  if( argc > 1 && argv[1][0] == 's' )
  {
    std::cerr<<"malloc single\n";
    pc_bench_st( do_malloc, do_malloc_free );
  }
  if( argc > 1 && argv[1][0] == 'S' )
  {
    std::cerr<<"hash malloc single\n";
    pc_bench_st( do_hash_malloc, do_hash_free );
  }
  return 0;
}


