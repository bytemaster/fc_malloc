#include "bit_index.hpp"



template<uint32_t ItemSize, uint32_t PageSize = 2*1024*1024>
class fixed_pool
{
  public:
    class page
    {
       public:
          page()
          {
            _free_blocks.set_all();
            _avail = PageSize / ItemSize;
          }

          char* alloc()
          {
              auto b = _free_blocks.first_set_bit();
              if( b < PageSize / ItemSize )
              {
                  --_avail;
                  _free_blocks.clear(b);
                  return &_data[b*ItemSize];
              }
              return nullptr;
          }

          int64_t available()const
          {
              return _avail;
          }

          bool free( char* c )
          {
              int64_t offset = c - _data;
              if( offset >= 0 && offset < PageSize )
              {
                 _free_blocks.set( offset / ItemSize );
                 ++_avail;
                 return true;
              }
              return false;
          }

          bool is_claimed()const
          {
            return 0 != _claim.load(0);
          }

          bool claim()
          {
            return 0 == _claim.fetch_add(1);
          }

          void release() 
          {
            _claim.store(0);
          }
       protected:
        int64_t             _gc_avail; // count managed by 

        int64_t             _alloc_avail; // count managed by alloc thread

        bit_index<64*64*64> _free_blocks; // updated by gc thread
        std::atomic<int>    _claim;

        bit_index<64*64*64> _alloc_blocks; // updated by active thread

        uint32_t            _page_num;
        char                _data[PageSize];
    }; // class page

    page* claim_page()
    {
        auto first_free = _free_pages.first_set_bit();
        do
        {
            if( _pages[first_free] == nullptr )
            {
                page* p = new page();
                int claim = _next_page.fetch_add(1);
                p->claim();
                _pages[claim] = p;
                return p;
            }
            if( _pages[first_free]->claim() )
              return _pages[first_free];

            first_free++; // = _free_pages.first_set_bit();
        } while( true ); 
    }

    typedef page*        page_ptr;
    std::atomic<int>     _next_page; // inc to allocate a new page.

    // updated by gc thread... 'unclaimed pages' with free data.
    bit_index<64*64*64>  _free_pages;
    page_ptr             _pages[64*32];
};


