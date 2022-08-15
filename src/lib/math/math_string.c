#include <stdarg.h>
#include <assert.h>
#include <string.h>
#include <stddef.h>

#define xGENERIC 0
#define xFLOAT 1
#define xEXP 2


/*
** The code that follow is based on "printf" code that dates from the
** 1980s. It is in the public domain.  The original comments are
** included here for completeness.  They are very out-of-date but
** might be useful as an historical reference.
*/

static char
getdigit(double *val, int *cnt)
{
  int digit;
  double d;
  if( (*cnt)++ >= 16 ) return '0';
  digit = (int)*val;
  d = digit;
  digit += '0';
  *val = (*val - d)*10.0;
  return (char)digit;
}


static int
dbl2str(char *buf, size_t bufsize, double realvalue, int precision)
{
  char *bufpt;
  char prefix;
  char xtype = xGENERIC;
  int idx, exp, e2;
  double rounder;
  char flag_exp;
  char flag_rtz;
  char flag_dp;
  char flag_alternateform = 0;
  char flag_altform2 = 0;
  int nsd;

  if(bufsize < 8)
    return -1;

  if( precision<0 ) precision = 20;         /* Set default precision */
  if( precision>bufsize/2-10 ) precision = bufsize/2-10;
  if( realvalue<0.0 ){
    realvalue = -realvalue;
    prefix = '-';
  }else{
    prefix = 0;
  }
  if( xtype==xGENERIC && precision>0 ) precision--;
  for(idx=precision, rounder=0.5; idx>0; idx--, rounder*=0.1){}

  if( xtype==xFLOAT ) realvalue += rounder;
  /* Normalize realvalue to within 10.0 > realvalue >= 1.0 */
  exp = 0;

#if 0
  if(isnan(realvalue)) {
    memcpy(buf, "NaN", 4);
    return 0;
  }
#endif

  if( realvalue>0.0 ){
    while( realvalue>=1e32 && exp<=350 ){ realvalue *= 1e-32; exp+=32; }
    while( realvalue>=1e8 && exp<=350 ){ realvalue *= 1e-8; exp+=8; }
    while( realvalue>=10.0 && exp<=350 ){ realvalue *= 0.1; exp++; }
    while( realvalue<1e-8 ){ realvalue *= 1e8; exp-=8; }
    while( realvalue<1.0 ){ realvalue *= 10.0; exp--; }
    if( exp>350 ){
      if( prefix=='-' ){
	memcpy(buf, "-Inf", 5);
      }else{
	memcpy(buf, "Inf", 4);
      }
      return 0;
    }
  }
  bufpt = buf;

  /*
  ** If the field type is etGENERIC, then convert to either etEXP
  ** or etFLOAT, as appropriate.
  */
  flag_exp = xtype==xEXP;
  if( xtype != xFLOAT ){
    realvalue += rounder;
    if( realvalue>=10.0 ){ realvalue *= 0.1; exp++; }
  }
  if( xtype==xGENERIC ){
    flag_rtz = !flag_alternateform;
    if( exp<-4 || exp>precision ){
      xtype = xEXP;
    }else{
      precision = precision - exp;
      xtype = xFLOAT;
    }
  }else{
    flag_rtz = 0;
  }
  if( xtype==xEXP ){
    e2 = 0;
  }else{
    e2 = exp;
  }
  nsd = 0;
  flag_dp = (precision>0 ?1:0) | flag_alternateform | flag_altform2;
  /* The sign in front of the number */
  if( prefix ){
    *(bufpt++) = prefix;
  }
  /* Digits prior to the decimal point */
  if( e2<0 ){
    *(bufpt++) = '0';
  }else{
    for(; e2>=0; e2--){
      *(bufpt++) = getdigit(&realvalue,&nsd);
    }
  }
  /* The decimal point */
  if( flag_dp ){
    *(bufpt++) = '.';
  }
  /* "0" digits after the decimal point but before the first
  ** significant digit of the number */
  for(e2++; e2<0; precision--, e2++){
    assert( precision>0 );
    *(bufpt++) = '0';
  }
  /* Significant digits after the decimal point */
  while( (precision--)>0 ){
    *(bufpt++) = getdigit(&realvalue,&nsd);
  }

  /* Remove trailing zeros and the "." if no digits follow the "." */
  if( flag_rtz && flag_dp ){
    while( bufpt[-1]=='0' ) *(--bufpt) = 0;
    assert( bufpt>buf );
    if( bufpt[-1]=='.' ){
      if( flag_altform2 ){
	*(bufpt++) = '0';
      }else{
	*(--bufpt) = 0;
      }
    }
  }
  /* Add the "eNNN" suffix */
  if( flag_exp || xtype==xEXP ){
    *(bufpt++) = 'e';
    if( exp<0 ){
      *(bufpt++) = '-'; exp = -exp;
    }else{
      *(bufpt++) = '+';
    }
    if( exp>=100 ){
      *(bufpt++) = (char)((exp/100)+'0');        /* 100's digit */
      exp %= 100;
    }
    *(bufpt++) = (char)(exp/10+'0');             /* 10's digit */
    *(bufpt++) = (char)(exp%10+'0');             /* 1's digit */
  }
  *bufpt = 0;
  return 0;
}



va_list
fmt_double(va_list ap, char *buf, size_t buflen, int prec)
{
  double d = va_arg(ap, double);
  dbl2str(buf, buflen, d, prec);
  return ap;
}
