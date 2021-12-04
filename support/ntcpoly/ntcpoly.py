import argparse
import numpy as np
import math
import sys

#
#
#  VCC --- [Rfixed] -+- [NTC] --- GND
#                    |
#  ADCin ------------+
#
#

class ADCNTC:
    def __init__(self, A, B, C, Rfixed, Vcc, ADCbits, tmin, tmax):
        self.A = A
        self.B = B
        self.C = C
        self.Rfixed = Rfixed
        self.Vcc = Vcc
        self.ADCbits = ADCbits
        self.xmin = self.rev(tmax)
        self.xmax = self.rev(tmin)

    def fwd(self, x):
        v = x * self.Vcc / (2 ** self.ADCbits)
        R = (self.Rfixed * v) / (self.Vcc - v)
        Rln = np.log(R)
        y = 1 / (self.A + self.B * Rln + self.C * Rln * Rln * Rln) - 273.15
        return y;

    # https://en.wikipedia.org/wiki/Steinhart%E2%80%93Hart_equation#Inverse_of_the_equation
    def rev(self, T):
        T = T + 273.15
        x = (self.A - (1.0 / T)) / self.C
        y = (((self.B / (3 * self.C)) ** 3) + (x / 2) ** 2) ** 0.5
        R = np.exp(((y - x / 2) ** (1./3.)) - (y + x / 2) ** (1./3.))
        v = self.Vcc * R / (self.Rfixed + R)
        x = v / self.Vcc * (2 ** self.ADCbits)
        return int(round(x))

    def values(self):
        return [(x, self.fwd(x)) for x in range(self.xmin, self.xmax)]

    def poly1d(self):
        xs = range(self.xmin, self.xmax)
        ys = [self.fwd(x) for x in xs]
        return np.poly1d(np.polyfit(xs, ys, deg=3))

    def get_shift(self, x):
        return 31 - self.ADCbits - math.ceil(math.log2(abs(x)))

    def calc_constants(self):
        p = self.poly1d()
        s = []
        K = []
        for i in range(0,4):
            s.append(self.get_shift(p[i]))
            K.append(int(round(p[i] * 2 ** s[i])))
        s[1] = s[1] - s[0]
        s[2] = s[2] - s[0] - self.ADCbits
        s[3] = s[3] - s[0] - self.ADCbits * 2
        self.K = K
        self.s = s

    def integer_calc(self, x):
        x2 = (x  * x) >> self.ADCbits
        x3 = (x2 * x) >> self.ADCbits
        t0 =  self.K[0]
        t1 = (self.K[1] * x ) >> self.s[1]
        t2 = (self.K[2] * x2) >> self.s[2]
        t3 = (self.K[3] * x3) >> self.s[3]
        return (t0 + t1 + t2 + t3) / 2 ** self.s[0]

    def get_c_struct(self, name):
        return """
const struct ntcpoly %s = {
  .K0 = %d, .K1 = %d, .K2 = %d, .K3 = %d,
  .s0 = %d, .s1 = %d, .s2 = %d, .s3 = %d, .r = %d
};
""" % (name,
       self.K[0], self.K[1], self.K[2], self.K[3],
       self.s[0], self.s[1], self.s[2], self.s[3],
       self.ADCbits);


parser = argparse.ArgumentParser(description='Compute ADC value to Temperature polynomial for NTC resistors')

parser.add_argument("A", help="Steinhart-Hart Coefficient A", type=float)
parser.add_argument("B", help="Steinhart-Hart Coefficient B", type=float)
parser.add_argument("C", help="Steinhart-Hart Coefficient C", type=float)
parser.add_argument("-r", "--resistance", help="Voltage divider resistance (default %(default)d Ohm)",
                    type=float, default=10000)
parser.add_argument("--vcc", help="Reference voltage (default %(default).1f V)",
                    type=float, default=3.3)
parser.add_argument("--bits", help="ADC bits (default %(default)d)",
                    type=int, default=12)
parser.add_argument("--min", help="Min temperature range for polyfit (default %(default)d °C)",
                    type=int, default=-30)
parser.add_argument("--max", help="Max temperature range for polyfit (default %(default)d °C)",
                    type=int, default=50)
parser.add_argument("--name", help="Name for C struct", default="ntcpoly")

args = parser.parse_args()
a = ADCNTC(args.A, args.B, args.C, args.resistance, args.vcc, args.bits, args.min, args.max)
a.calc_constants()

print("""
struct ntcpoly {
  int32_t K0, K1, K2, K3;
  uint8_t s0, s1, s2, s3, r;
};


int ntcpoly_compute(int32_t x, const struct ntcpoly *np) {
  const int32_t x2 = (x * x) >> np->r;
  const int32_t x3 = (x2 * x) >> np->r;
  const int32_t t0 = np->K0;
  const int32_t t1 = (np->K1 * x)  >> np->s1;
  const int32_t t2 = (np->K2 * x2) >> np->s2;
  const int32_t t3 = (np->K3 * x3) >> np->s3;
  return (t0 + t1 + t2 + t3) >> np->s0;
}

""")

print(a.get_c_struct(args.name));
