/*
 *      $Id$
 */
/************************************************************************
*									*
*			     Copyright (C)  2002			*
*				Internet2				*
*			     All Rights Reserved			*
*									*
************************************************************************/
/*
 *	File:		arithm128.c
 *
 *	Author:		Anatoly Karp
 *			Internet2
 *
 *	Date:		Sun Jun 02 12:29:20 MDT 2002
 *
 *	Description:	Implementation of extended-precision arithmetic.
 *                      It uses eight 16-bit quantities to represent
 *                      and manipulate Owamp's [64].[64] space of timestamps.
 */

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include "arithm128.h"
#include "rijndael-api-fst.h"

/* we often need to scale by 10^6 so let's fix a struct for that */
static struct num_128 million = {0, 0, 0, 0, 16960, 15, 0, 0};

#define K 19
static unsigned long Q[K][2] = {
	0x0,        0x0,             /* fake */
	0xB17217F7, 0xD1CF79AB,
	0xEEF193F6, 0xD75A96FC,
	0xFD271862, 0x59AAF6C2,
	0xFF9D6DD0, 0x50F4C5A7,
	0xFFF4CFCF, 0xEF1E626B,
	0xFFFEE818, 0x86E1C62F,
	0xFFFFE7FE, 0x850E0BB5,
	0xFFFFFE2A, 0x8731B17D,
	0xFFFFFFDF, 0xAC6EEADC,
	0xFFFFFFFD, 0xF9640067,
	0xFFFFFFFF, 0xE22EC79C,
	0xFFFFFFFF, 0xFE6A9DED,
	0xFFFFFFFF, 0xFFEBFF81,
	0xFFFFFFFF, 0xFFFF1417,
	0xFFFFFFFF, 0xFFFFF5CE,
	0xFFFFFFFF, 0xFFFFFF95,
	0xFFFFFFFF, 0xFFFFFFFB,
	0xFFFFFFFF, 0xFFFFFFFF
};

/*
** Constructor function. Create a num struct out of its components.
** In the argument, more significant digits go first. 
** Thus, (a, b, c, d, e, f, g, h) gives rise to the number
**        a*(2^48) + b*(2^32) + c*(2^16) + d + 
**        e/(2^16) + f/(2^32) + g*(2^48) + h/(2^64)
*/
struct num_128 num_new(unsigned short a,
		       unsigned short b,
		       unsigned short c,
		       unsigned short d,
		       unsigned short e,
		       unsigned short f,
		       unsigned short g,
		       unsigned short h)
{
	struct num_128 x = { {0, 0, 0, 0, 0, 0, 0, 0} };
	
	x.digits[0] = h;
	x.digits[1] = g;
	x.digits[2] = f;
	x.digits[3] = e;
	x.digits[4] = d;
	x.digits[5] = c;
	x.digits[6] = b;
	x.digits[7] = a;
	
	return x;
}

#define BASE 0x10000 /* i.e., arithmetic is done modulo 2^16 */

/* 
** This function embeds the unsigned 32-bit integers into the
** num space.
*/
struct num_128
ulong2num(unsigned long a)
{
	int i;
	struct num_128 ret;
	
	for (i = 0; i < NUM_DIGITS; i++)
		ret.digits[i] = 0;

	ret.digits[5] = (unsigned short)(a >> 16);
	ret.digits[4] = (unsigned short)(a & 0xffff);

	return ret;
}

/*
** This function is used primarily for debugging.
*/
void
num_print(num_128 x)
{
	int i;
	assert(x);
	
	for (i = NUM_DIGITS - 1; i >= 0; i--)
		fprintf(stderr, "%hu ", x->digits[i]);
	fprintf(stderr, "\n");
}

/*
** The next two functions implement the basic arithmetic
** operation in the num space. The result is saved
** in the variable z.
*/
void 
num_add(num_128 x, num_128 y, num_128 z)
{
	int i;
	unsigned short carry = 0;

	assert(x); assert(y); assert(z);
	for (i = 0; i < NUM_DIGITS; i++){
		/* now carry is 0 or 1 */
		z->digits[i] = x->digits[i] + y->digits[i];

		if(z->digits[i] < x->digits[i] || z->digits[i] < y->digits[i]){
 			z->digits[i] += carry;  /* overflow happened */
			carry = 1;
		} else {
			if (!carry)
				continue;
			z->digits[i] += carry;

			/* now only need to update carry */
			if (z->digits[i] != 0)
				carry = 0;
		}
	}
}

void 
num_mul(num_128 x, num_128 y, num_128 z)
{
	int i, j;
	unsigned short tmp[(2*NUM_DIGITS)];
	
	assert(x); assert(y); assert(z);
	for (i = 0; i < (2*NUM_DIGITS); i++)
		tmp[i] = 0UL;

	for (i = 0; i < NUM_DIGITS; i++){
		unsigned long int carry = 0;
		for (j = 0; j < NUM_DIGITS; j++){
			carry += x->digits[i]*y->digits[j] + tmp[i+j];
			tmp[i+j] = carry & 0xffff;
			carry >>= 16;
		}

		for ( ; j < (2*NUM_DIGITS) - i; j++){
			carry += tmp[i+j];
			tmp[i+j] = carry & 0xffff;
			carry >>= 16;
		}
	}

	/* Need to shift by NUM_DIGITS/2 digits now */
	for (i = 0; i < NUM_DIGITS; i++)
		z->digits[i] = tmp[i+(NUM_DIGITS/2)];
}

/*
** The next two functions perform conversion between the num space
** and Owamp 32/24 space of timestamps (32-bit number of integer
** seconds, and 24-bit number of fractional seconds).
*/

/*
** Discussion: abcd.efgh = A.B, where LHS is in base 2^16,
** 0 <= A <= 2^32 - 1, 0 <= B <= 2^24 - 1, then
** A = c*(2^16) + d, and (multiplying fractional parts by 2^24)
** B = e*(2^8) + f/(2^8) [commit to always rounding down - shifting]
*/
void
num2formatted(num_128 from, OWPFormattedTime to)
{
	to->t[0] = (unsigned long)(from->digits[5]) << 16 + from->digits[4];
	to->t[1] = (unsigned long)(from->digits[3]) << 8 
		+ (unsigned long)(from->digits[2])>> 8;
	to->t[1] <<= 8; /* place the result into 24 most significant bits */
}

/*
** Discussion: only handling of the fractional parts is interesting.
** Let e/(2^16) + f/(2^32) = B/(2^24), then (multiplying by 2^32)
** e*(2^16) + f = B*(2^8) [which is the same as from->t[1] below].
** Thus both e and f can be recovered by performing division
** with remainder by 2^16.
*/
void
formatted2num(OWPFormattedTime from, num_128 to)
{
	to->digits[7] = to->digits[6] = 0;
	to->digits[5] = (unsigned short)(from->t[0] >> 16);
	to->digits[4] = (unsigned short)(from->t[0] & 0xffff);

	/* the fractional part has been left-shifted by 8 bits already */
	to->digits[3] = (unsigned short)(from->t[1] >> 16);
	to->digits[2] = (unsigned short)(from->t[1] & 0xffff);
	to->digits[1] = to->digits[0] = 0;
}

/* 
** The next two functions perform conversion between the num space and
** Unix timeval structs.
*/

void 
num2timeval(num_128 from, struct timeval *to)
{
	struct num_128 res;

	/* first convert the fractional part */
	unsigned short a = from->digits[3];
	unsigned short b = from->digits[2];
	struct num_128 tmp = num_new(0, 0, 0, 0, a, b, 0, 0);

	num_mul(&tmp, &million, &res);
	to->tv_usec = res.digits[4];

	/* now the integer part */
	to->tv_sec = (unsigned long)(from->digits[5]) << 16 + from->digits[4];
}

void 
timeval2num(struct timeval *from, num_128 to)
{

}

/*
** This function treats a num struct as representing an unsigned long
** integer, and returns that integer.
*/
unsigned long 
num2ulong(num_128 x)
{
	return (x->digits[5] << 16) + x->digits[4];
}

/*
** This function treats a num struct as representing an unsigned long long
** integer, and returns that integer. 
**
** NOTE: used for debugging only - not included in the final distribution.
*/
unsigned long long 
num2ulonglong(num_128 x)
{
	unsigned long long ret = ((unsigned long long)(x->digits[7]) << 48)
	        + ((unsigned long long)(x->digits[6]) << 32)
		+ ((unsigned long long)(x->digits[5]) << 16)
		+ x->digits[4]; 

	return ret;
}

/*
** This function converts an unsigned long long integer into a num struct.
**
** NOTE: used for debugging only - not included in the final distribution.
*/
struct num_128
ulonglong2num(unsigned long long a)
{
	int i;
	struct num_128 ret;
	
	for (i = 0; i < NUM_DIGITS; i++)
		ret.digits[i] = 0;

	ret.digits[7] = (unsigned short)(a >> 48);
	ret.digits[6] = (unsigned short)((a & 0xffff00000000) >> 32);
	ret.digits[5] = (unsigned short)((a & 0xffff0000) >> 16);
	ret.digits[4] = (unsigned short)(a & 0xffff);

	return ret;
}

/*
** This function converts a 64-bit binary string (network byte order)
** into num struct (fractional part).
*/
struct num_128
raw2num(unsigned char *raw)
{
	unsigned short dig[4];
	int i;

	for (i = 0; i < 4; i++)
		dig[i] = (((unsigned short)(*(raw + 2*i))) << 8) 
			+ *(raw + 2*i + 1); 

	return num_new(0, 0, 0, 0, ntohs(dig[0]), ntohs(dig[1]), 
		       ntohs(dig[2]), ntohs(dig[3]));
}

/* 
** Generate an exponential deviate using 64-bit binary string as an input
** (encoded using 2 unsigned long integers). This is algorithm S from
** Knuth's v.2 of "Art of Computer Programming" (1998), p.133.
*/
struct num_128 
random_exp(keyInstance *key, unsigned long in)
{
	struct num_128 ret;
	int i, j, k, count;
	BYTE outBuffer[16];
	struct num_128 rand_uniform; /* represents 64-bit uniform deviate */
	unsigned long high, low;       /* network byte ordered rand_uniform */

	if (countermodeEncrypt(key, in, outBuffer) != 128){
		fprintf(stderr, "FATAL: counter mode encryption error");
		exit(1);
	}

	/* Grab the last 8 bytes of the encrypted block */
	rand_uniform = raw2num(outBuffer + 8);

	/* 	memcpy(rand_uniform, outBuffer + 8, 8); */

	high = htonl(rand_uniform[0]);
	low  = htonl(rand_uniform[1]);

	/* Get U and shift */
	for (i = 0; i < 2; i++)
		for (j = 0; j < 32; j++){
			
		}

	/* Immediate acceptance? */
	if (high < Q[1][0] || (high == Q[1][0] && low < Q[1][1])){

		/*
		  ...
		*/
		
		return ret;
	}

	/* Minimize */
	for (k = 2; k < K; k++)
		if (high < Q[k][0] || (high == Q[k][0] && low < Q[k][1]))
			break;

	if (k == K){
		fprintf(stderr, "FATAL: random_exp");
		exit(1);
	}

	/* Generate k new uniform deviates */

	/*
	  ...
	*/

	return ret;
}
