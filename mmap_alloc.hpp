#pragma once
#include <algorithm>
extern "C" {
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <math.h>
}


size_t pagesize()
{
  return ::getpagesize();
}
size_t page_count( size_t s )
{
    return static_cast< size_t >( ceilf( static_cast< float >( s) / pagesize() ) );
}

char* mmap_alloc( size_t s, void* loc = 0 )
{
   //fprintf( stderr, "mmap_alloc %llu   %p\n", s, loc );
   const std::size_t pages( page_count(s) ); // add +1 for guard page
   std::size_t size_ = pages * pagesize();
   
   # if defined(macintosh) || defined(__APPLE__) || defined(__APPLE_CC__)
    void* limit = ::mmap( loc, size_, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
   # else
    const int fd( ::open("/dev/zero", O_RDONLY) );
    assert( -1 != fd);
    void* limit = ::mmap( loc, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
   # endif
   if ( !limit ) throw std::bad_alloc();
   return static_cast<char*>(limit);
}

void mmap_free( void* pos, size_t s )
{
   const std::size_t pages( page_count( s) ); // add +1 for guard page
   std::size_t size_ = pages * pagesize();
   ::munmap( pos, size_);
}
