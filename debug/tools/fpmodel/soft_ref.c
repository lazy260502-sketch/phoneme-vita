/* same functions with the soft-float calling convention (doubles in r0-r3) */
__attribute__((pcs("aapcs")))
double dmul_s(double x, double y) { return x * y; }
__attribute__((pcs("aapcs")))
int    d2i_s(double x)            { return (int)x; }
__attribute__((pcs("aapcs")))
double i2d_s(int x)               { return (double)x; }
