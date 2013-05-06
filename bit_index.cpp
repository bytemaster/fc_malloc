#include "bit_index.hpp"
#include <stdio.h>

int main( int argc, char** argv )
{

    bit_index<64*64*64> b;
    b.set_all();
    for( int i = 0; i < 66; ++i )
    {
      b.clear(i);
      assert( !b.get(i) );
      fprintf( stderr, "\nI: %d\n", i );
      if( i >= 62 )
          b.dump();
      if( b.first_set_bit() != i+1 )
      {
          exit(1);
      }
    }
    for( int i = 0; i < 66; ++i )
    {
      assert( !b.get(i) );
    }
    assert( b.get(67) );

    return 0;
    fprintf( stderr, "pow64(1) = %d\n", pow64<1>::value );
    fprintf( stderr, "pow64(2) = %d\n", pow64<2>::value );
    fprintf( stderr, "log64(pow64(2)) = %d\n", log64<pow64<2>::value>::value );
    fprintf( stderr, "pow64(log64(64*64)) = %d\n", pow64<log64<64*64>::value>::value );
    fprintf( stderr, "pow64(log64(64*64*64)) = %d\n", pow64<log64<64*64*64>::value>::value );

    fprintf(stderr, "=========== 64 =============\n" ); 
    bit_index<64> _index;
    fprintf( stderr, "first set bit: %d\n", _index.first_set_bit() );
    assert( _index.first_set_bit() == 64 );
    _index.set( 34 );
    fprintf( stderr, "first set bit: %d\n", _index.first_set_bit() );
    assert( _index.get(34) );
    assert( _index.first_set_bit() == 34 );
    _index.clear(34);
    assert( !_index.get(34) );
    assert( _index.first_set_bit() == 64 );
    fprintf(stderr, "=========== 64*64 =============\n" ); 

    bit_index<64*64> _b62;
    _b62.set(1010);
    fprintf( stderr, "first set bit: %d\n", _b62.first_set_bit() );
    assert( _b62.first_set_bit() == 1010 );
    assert( _b62.get(1010) );
    assert( _b62.clear(1010) );
    assert( !_b62.get(1010) );

    fprintf(stderr, "=========== 64*64*64 =============\n" ); 
    bit_index<64*64*64> _b64;
    fprintf( stderr, "init first bit b64: %d\n", _b64.first_set_bit() );
    _b64.set( 660 );
    fprintf( stderr, "first set:   %d\n", _b64.first_set_bit() );
    assert( _b64.get(660) );
    _b64.clear(660);
    fprintf( stderr, "final first bit b64: %d\n", _b64.first_set_bit() );
    assert( !_b64.get(660) );

    bit_index<64*64*64> _b6464;
    fprintf( stderr, "SET BIT 66\n" );
    _b6464.set( 66 );
    fprintf( stderr, "first set 66?? :   %d\n", _b6464.first_set_bit() );
    fprintf( stderr, "size of %d 64*64*64\n", int(sizeof(_b64) ) );

    bit_index<64*64*64*64> _bbb;
    fprintf( stderr, "size of %d  64*64*64*64  \n\n\n", int(sizeof(_bbb) ) );
    _bbb.set(444);
    assert(_bbb.get(444) );
    {
    bit_index<64*64> _bbb;
    fprintf( stderr, "size of %d  64*64*64*64  \n\n\n", int(sizeof(_bbb) ) );
    _bbb.set(444);
    assert(_bbb.get(444) );
    }
    /*
    {
    bit_index<20*64*64> _bbb;
    fprintf( stderr, "size of %d  64*64*64*64  \n\n\n", int(sizeof(_bbb) ) );
    _bbb.set(444);
    assert(_bbb.get(444) );
    }
    */

    _index.set(3);
    _index.set(9);
    _index.set(27);

    auto itr = _index.at( _index.first_set_bit() );
    while( !itr.end() )
    {
       fprintf( stderr, "next bit %lld\n", itr.bit() );
       itr.next_set_bit();
    }

  {
    _b62.set(3);
    _b62.set(9);
    _b62.set(270);
    _b62.set(570);
    _b62.set(1270);

    auto itr = _b62.at( _b62.first_set_bit() );
    while( !itr.end() )
    {
       fprintf( stderr, "_b62 next bit %lld\n", itr.bit() );
       itr.next_set_bit();
    }
  }

    auto tmp = _bbb.begin();
    return 0;
}
