/* TJpgDec settings for PalmView, used in place of the release's
 * tjpgdcnf.h. */

/* Stream input buffer */
#define JD_SZBUF 512
/* RGB565 output */
#define JD_FORMAT 1
/* Output scaling by 1/2, 1/4 and 1/8 */
#define JD_USE_SCALE 1
/* Table-based clipping: faster, 1 KB more code */
#define JD_TBLCLIP 1
/* Basic optimization only: the 68K build has 16-bit int. */
#define JD_FASTDECODE 0
