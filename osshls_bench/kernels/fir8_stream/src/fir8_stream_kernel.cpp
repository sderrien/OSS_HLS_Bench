#include <ohls_stream.h>

#pragma HLS top
extern "C" int fir8_stream(ohls::stream_in<int> &in, ohls::stream_out<int> &out) {

  int x0 = 0;
  int x1 = 0;
  int x2 = 0;
  int x3 = 0;
  int x4 = 0;
  int x5 = 0;
  int x6 = 0;
  int x7 = 0;

  int cpt = 0x100;
#pragma HLS pipeline
  while (cpt-- > 0) {
    int s = in.read();
    x7 = x6;
    x6 = x5;
    x5 = x4;
    x4 = x3;
    x3 = x2;
    x2 = x1;
    x1 = x0;
    x0 = s;
    int sum = x0 * 1 + x1 * 2 + x2 * 3 + x3 * 4 + x4 * 5 + x5 * 6 + x6 * 7 + x7 * 8;
    out.write(sum);
  }
  return 0;
}
