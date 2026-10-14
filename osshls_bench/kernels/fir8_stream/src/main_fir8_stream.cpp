#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <ohls_stream.h>

// Declare the FIR kernel with C linkage so simulators can find the unmangled symbol.
extern "C" int fir8_stream(ohls::stream_in<int> &in, ohls::stream_out<int> &out);

int fir8_stream_golden(ohls::stream_in<int> &in, ohls::stream_out<int> &out) {

  int cpt = 0x100;
  int c[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  int x[8] = {0};
  int k=0;
  while(cpt-- > 0) {
    int sum = 0;
    x[k] = in.read();
    for (int i = 0; i < 8; ++i)
    {
      sum += x[(k - i) & 7] * c[i];
    }
    k = (k + 1) & 7;
    out.write(sum);
  }
  return 0;
}



int main(int argc, char **argv) {
  FILE *fout = stdout;
  if (argc > 1) {
    fout = fopen(argv[1], "w");
    if (!fout) {
      perror("fopen");
      return 1;
    }
  }

  ohls::stream<int, 256> buffer_in;
  ohls::stream<int, 256> buffer_out;
  
  ohls::stream_in<int> in(buffer_in);
  ohls::stream_out<int> out_s(buffer_out);

  ohls::stream<int, 256> buffer_in_golden;
  ohls::stream<int, 256> buffer_out_golden;
  
  ohls::stream_in<int> in_golden(buffer_in_golden);
  ohls::stream_out<int> out_golden(buffer_out_golden);

 

  for (int i = 0; i < 256; ++i) {
    buffer_in.write(i);
    buffer_in_golden.write(i);
  }

  int goldenn = fir8_stream_golden(in_golden, out_golden);
  int dute = fir8_stream(in, out_s);

  int nberrors = 0;
  for (int i = 0; i < 256; ++i) {
    int golden = buffer_out_golden.read();
    int actual = buffer_out.read();
    if (golden != actual) {
      nberrors++;
    }
  }

  fprintf(fout, "errors=%d\n", nberrors);
  fprintf(fout, "match=%s\n", nberrors == 0 ? "yes" : "no");

  if (fout != stdout) {
    fclose(fout);
  }
  return nberrors == 0 ? 0 : 2;
}
