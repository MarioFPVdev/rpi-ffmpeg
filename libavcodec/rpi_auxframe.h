
struct AVFrame;
struct AVBufferRef;

uint8_t * rpi_gpu_buf_data_arm(const struct AVBufferRef * const buf);
struct AVBufferRef * rpi_gpu_buf_alloc(const unsigned int numbytes, const int flags);


#define RPI_AUX_FRAME_XBLK_SHIFT 4  // blk width 16
#define RPI_AUX_FRAME_XBLK_WIDTH (1 << RPI_AUX_FRAME_XBLK_SHIFT)

#define RPI_AUX_FRAME_BUF_NO (AV_NUM_DATA_POINTERS - 1)

#define RPI_AUX_FRAME_TEST 1

typedef struct RpiAuxframeDesc
{
    struct AVBufferRef * buf;
    uint8_t * data_y;
    uint8_t * data_c;
    unsigned int stride;
} RpiAuxframeDesc;

static inline const RpiAuxframeDesc * rpi_auxframe_desc(const AVFrame * const frame)
{
    return (const RpiAuxframeDesc *)(frame->buf[RPI_AUX_FRAME_BUF_NO] == NULL ? NULL :
        frame->buf[RPI_AUX_FRAME_BUF_NO]->data);
}

static inline uint8_t * rpi_auxframe_ptr_y(const RpiAuxframeDesc * const d, const unsigned int x, const unsigned int y)
{
    return d == NULL ? NULL : d->data_y +
        (x & (RPI_AUX_FRAME_XBLK_WIDTH - 1)) +
        (y << RPI_AUX_FRAME_XBLK_SHIFT) +
        (x >> RPI_AUX_FRAME_XBLK_SHIFT) * d->stride;
}

// X,Y in chroma coords
static inline uint8_t * rpi_auxframe_ptr_c(const RpiAuxframeDesc * const d, const unsigned int x, const unsigned int y, const unsigned int c)
{
    return d == NULL ? NULL : d->data_c +
        (x & (RPI_AUX_FRAME_XBLK_WIDTH / 2 - 1)) +
        (y << RPI_AUX_FRAME_XBLK_SHIFT) +
        (x >> (RPI_AUX_FRAME_XBLK_SHIFT - 1)) * (d->stride >> 1) +
        (c << (RPI_AUX_FRAME_XBLK_SHIFT - 1));
}

#define rpi_auxframe_ptr_u(d,x,y) rpi_auxframe_ptr_c(d,x,y,0)
#define rpi_auxframe_ptr_v(d,x,y) rpi_auxframe_ptr_c(d,x,y,1)

int rpi_auxframe_attach(struct AVFrame * const frame);

