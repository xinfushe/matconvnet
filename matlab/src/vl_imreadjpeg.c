/** @file vl_nnconv.cu
 ** @brief Convolution block
 ** @author Andrea Vedaldi
 **/

/*
 Copyright (C) 2014 Andrea Vedaldi and Max Jaderberg.
 All rights reserved.

 This file is part of the VLFeat library and is made available under
 the terms of the BSD license (see the COPYING file).
 */

#include "bits/mexutils.h"

#include <stdio.h>
#include <jpeglib.h>
#include <pthread.h>

/* option codes */
enum {
  opt_num_threads = 0,
  opt_prefetch,
  opt_verbose,
} ;

/* options */
vlmxOption  options [] = {
  {"NumThreads",       1,   opt_num_threads        },
  {"Prefetch",         0,   opt_prefetch           },
  {"Verbose",          0,   opt_verbose            },
  {0,                  0,   0                      }
} ;


enum {
  IN_FILENAMES = 0, IN_END
} ;

enum {
  OUT_IMAGES = 0, OUT_END
} ;

/* ---------------------------------------------------------------- */
/*                                                           Reader */
/* ---------------------------------------------------------------- */

typedef struct QueuedImage_ {
  struct QueuedImage_ * next ;
  struct QueuedImage_ * previous ;
  char * filename ;
  size_t width ;
  size_t height ;
  size_t depth ;
  void * buffer ;
  bool locked ;
  int error ;
} QueuedImage ;

pthread_cond_t queueWait ;
pthread_mutex_t queueMutex ;
QueuedImage * queueFirst = NULL ;
QueuedImage * queueLast = NULL ;
QueuedImage * queueNextToRead = NULL ;

QueuedImage* queue_find (char const * filename)
{
  QueuedImage* image = queueFirst ;
  while (image) {
    if (strcmp(image->filename, filename) == 0) {
      /* this is a match */
      return image ;
    }
    image = image->next ;
  }
  return NULL ;
}

void queue_add (QueuedImage * image)
{
  image->previous = NULL ;
  image->next = NULL ;
  if (queueFirst == NULL) { /* empty queue */
    queueFirst = image ;
    queueLast = image ;
  } else {
    queueLast->next = image ;
    image->previous = queueLast ;
    queueLast = image ;
  }
}

void queue_remove (QueuedImage * image)
{
  if (queueFirst == image) {
    queueFirst = image->next ;
  }
  if (queueLast == image) {
    queueLast = image->previous ;
  }
  if (image->next) {
    image->next->previous = image->previous ;
  }
  if (image->previous) {
    image->previous->next = image->next ;
  }
}

typedef struct Reader_
{
  struct jpeg_decompress_struct decompressor ;
  struct jpeg_error_mgr standardErrorManager ;
} Reader ;

void reader_init (Reader* self)
{
  self->decompressor.err = jpeg_std_error(&self->standardErrorManager) ;
  jpeg_create_decompress(&self->decompressor) ;
  self->decompressor.out_color_space = JCS_RGB ;
  self->decompressor.quantize_colors = FALSE ;
}

void reader_deinit (Reader* self)
{
  jpeg_destroy_decompress(&self->decompressor) ;
}

void reader_read (Reader* self, QueuedImage * image)
{
  JSAMPARRAY scanlines ;
  JSAMPROW scanline ;
  int row_stride ;

  /* open file */
  FILE* fp = fopen(image->filename, "r") ;
  if (fp == NULL) {
    image->error = -1 ;
    image->buffer = malloc(sizeof(char)*4096) ;
    snprintf(image->buffer, 4096, "vl_imreadjpeg: could not open file '%s'\n", image->filename) ;
    return ;
  }

  /* set which file to read */
  jpeg_stdio_src(&self->decompressor, fp);

  /* get image size and allocate buffer */
  jpeg_read_header(&self->decompressor, TRUE);
  image->width = self->decompressor.image_width ;
  image->height = self->decompressor.image_height ;
  image->depth = self->decompressor.num_components ;
  image->buffer = malloc(sizeof(float)*image->depth*image->width*image->height) ;

  /* start decompressing (this sets the output_* fields) */
  jpeg_start_decompress(&self->decompressor);

  /* allocate scaline buffer */
  row_stride = self->decompressor.output_width * self->decompressor.output_components ;
  scanlines = (*self->decompressor.mem->alloc_sarray)
  ((j_common_ptr) &self->decompressor, JPOOL_IMAGE, row_stride, 1);

  /* decompress each scanline and transpose result into MATLAB format */
  {
    /*
     output_scanline points to the next scaline to be read; it is incremented
     after read_scanline
     */
    while(self->decompressor.output_scanline < self->decompressor.output_height) {
      jpeg_read_scanlines(&self->decompressor, scanlines, 1);
      scanline = scanlines[0] ;
      JSAMPROW end = scanline
      + self->decompressor.output_components
      * self->decompressor.output_width ;
      int y = self->decompressor.output_scanline - 1 ;
      if (self->decompressor.output_components == 3) {
        float* r = (float*)image->buffer + 0 * (image->height*image->width) + y ;
        float* g = (float*)image->buffer + 1 * (image->height*image->width) + y ;
        float* b = (float*)image->buffer + 2 * (image->height*image->width) + y ;
        while (scanline != end) {
          *r = ((float) (*scanline++)) / 255.0f ;
          *g = ((float) (*scanline++)) / 255.0f ;
          *b = ((float) (*scanline++)) / 255.0f ;
          r += image->height ;
          g += image->height ;
          b += image->height ;
        }
      } else if (self->decompressor.output_components == 1) {
        float* v = (float*)image->buffer + 0 * (image->height*image->width) + y ;
        while (scanline != end) {
          *v = ((float) (*scanline++)) / 255.0f ;
          v += image->height ;
        }
      } else {
        assert(false) ;
      }
    }
  }
  jpeg_finish_decompress(&self->decompressor) ;
  fclose(fp) ;
}

/* ---------------------------------------------------------------- */
/*                                                            Cache */
/* ---------------------------------------------------------------- */

#define MAX_NUM_THREADS 128
Reader* readers [MAX_NUM_THREADS+1] ;
pthread_t threads [MAX_NUM_THREADS] ;
size_t numReaders = 0 ;
bool terminate ;

void * thread_function(void* reader_)
{
  Reader* reader = (Reader*) reader_ ;
  pthread_mutex_lock(&queueMutex) ;
  while (!terminate) {
    QueuedImage *image = queueFirst ;
    while (image) {
      if (!image->locked && image->buffer == NULL) {
        break ;
      }
      image = image->next ;
    }

    if (image == NULL) {
      pthread_cond_wait(&queueWait, &queueMutex) ;
    } else {
      image->locked = true ;
      pthread_mutex_unlock(&queueMutex) ;
      reader_read(reader, image) ;

      pthread_mutex_lock(&queueMutex) ;
      image->locked = false ;
      pthread_cond_broadcast(&queueWait) ;
    }
  }
  pthread_mutex_unlock(&queueMutex) ;
  return NULL ;
}

void delete_readers()
{
  /* terminate threads */
  pthread_mutex_lock(&queueMutex) ;
  terminate = true ;
  pthread_cond_broadcast(&queueWait) ; /* allow waiting threads to wake up and terminate */
  pthread_mutex_unlock(&queueMutex) ;
  void * status ;
  for (int t = 0 ; t < (signed)numReaders - 1 ; ++t) {
    pthread_join(threads[t] , &status) ;
  }
  pthread_cond_destroy(&queueWait) ;
  pthread_mutex_destroy(&queueMutex) ;

  for (int r = 0 ; r < numReaders ; ++r) {
    if (readers[r]) {
      reader_deinit(readers[r]);
      free(readers[r]) ;
      readers[r] = NULL ;
    }
  }
  numReaders = 0 ;
}

void create_readers(int requestedNumReaders)
{
  if (numReaders == requestedNumReaders) {
    return ;
  }

  /* reset */
  delete_readers() ;

  /* allocate plus one readers */
  for (int r = 0 ; r < requestedNumReaders ; ++r) {
    readers[r] = malloc(sizeof(Reader)) ;
    reader_init(readers[r]) ;
  }
  numReaders = requestedNumReaders ;

  /* start threads */
  pthread_mutex_init(&queueMutex, NULL) ;
  pthread_cond_init(&queueWait, NULL) ;
  terminate = false ;
  for (int t = 0 ; t < numReaders - 1 ; ++t) {
    pthread_create(threads + t, NULL, thread_function, readers[t+1]) ;
  }
}


void atExit()
{
  delete_readers() ;
}

/* ---------------------------------------------------------------- */
/*                                                            Cache */
/* ---------------------------------------------------------------- */

void mexFunction(int nout, mxArray *out[],
                 int nin, mxArray const *in[])
{

  bool prefetch = false ;
  int requestedNumThreads = 0 ;
  int verbosity = 0 ;
  int opt ;
  int next = IN_END ;
  mxArray const *optarg ;

  /* -------------------------------------------------------------- */
  /*                                            Check the arguments */
  /* -------------------------------------------------------------- */

  mexAtExit(atExit) ;

  if (nin < 1) {
    mexErrMsgTxt("There is less than one arguments.") ;
  }

  while ((opt = vlmxNextOption (in, nin, options, &next, &optarg)) >= 0) {
    switch (opt) {
      case opt_verbose :
        ++ verbosity ;
        break ;

      case opt_prefetch :
        prefetch = true ;
        break ;

      case opt_num_threads :
        requestedNumThreads = mxGetScalar(optarg) ;
        break ;
    }
  }

  if (!mxIsCell(in[IN_FILENAMES])) {
    mexErrMsgTxt("FILENAMES is not a cell array of strings.") ;
  }

  if (requestedNumThreads < 0 || requestedNumThreads > MAX_NUM_THREADS) {
    mexErrMsgTxt("NUMTHREADS is not between 0 and 128.") ;
  }

  create_readers(requestedNumThreads + 1) ;

  if (verbosity) {
    mexPrintf("vl_imreadjpeg: numThreads = %d\n", (signed)numReaders-1) ;
    pthread_mutex_lock(&queueMutex) ;
    int num = 0 ;
    for (QueuedImage * image = queueFirst ; image ; image = image->next) {
      num++ ;
      if (verbosity > 1) {
        mexPrintf("vl_imreadjpeg: cached image %d; loading %d, loaded %d, ('%s')\n",
                  num, image->locked, image->buffer != NULL, image->filename) ;
      }
    }
    mexPrintf("vl_imreadjpeg: %d images cached\n", num) ;
    pthread_mutex_unlock(&queueMutex) ;
  }

  if (!prefetch) {
    out[OUT_IMAGES] = mxCreateCellArray(mxGetNumberOfDimensions(in[IN_FILENAMES]),
                                        mxGetDimensions(in[IN_FILENAMES])) ;

  }

  /* fill queue */
  pthread_mutex_lock(&queueMutex) ;
  for (int i = 0 ; i < mxGetNumberOfElements(in[IN_FILENAMES]) ; ++i) {
    mxArray* filename_array = mxGetCell(in[IN_FILENAMES], i) ;
    if (!vlmxIsString(filename_array,-1)) {
      mexErrMsgTxt("FILENAMES contains an entry that is not a string.") ;
    }
    char filename [4096] ;
    mxGetString (filename_array, filename, sizeof(filename)/sizeof(char)) ;

    QueuedImage *image = queue_find(filename) ;
    if (image == NULL) {
      /* this image was not already enqueued */
      image = calloc(sizeof(QueuedImage),1) ;
      image->filename = malloc(strlen(filename)+1) ;
      strcpy(image->filename, filename) ;
      queue_add (image) ;
      if (verbosity > 1) {
        mexPrintf("vl_imreadjpeg: enqueued '%s'\n", image->filename) ;
      }
    }
  }
  pthread_mutex_unlock(&queueMutex) ;
  pthread_cond_signal(&queueWait) ;

  /* empty the queue */
  if (prefetch) return  ;

  for (int i = 0 ; i < mxGetNumberOfElements(in[IN_FILENAMES]) ; ++i) {
    mxArray* filename_array = mxGetCell(in[IN_FILENAMES], i) ;
    char filename [4096] ;
    mxGetString (filename_array, filename, sizeof(filename)/sizeof(char)) ;

    pthread_mutex_lock(&queueMutex) ;
    QueuedImage *image = queue_find(filename) ;
    assert(image) ;
    if (image->buffer == NULL && numReaders == 1) {
      /* no multithreading, read directly */
      queue_remove(image) ;
      pthread_mutex_unlock(&queueMutex) ;
      reader_read(readers[0], image) ;
    } else {
      while (image->locked || image->buffer == NULL) {
        if (verbosity > 1) {
          mexPrintf("vl_imreadjpeg: waiting for thread to finish reading '%s'\n", image->filename) ;
        }
        pthread_cond_wait(&queueWait, &queueMutex); /* unlock, wait, relock */
      }
      queue_remove(image) ;
      pthread_mutex_unlock(&queueMutex) ;
    }

    /* now the image is read */
    if (image->error) {
      mexWarnMsgTxt((char*)image->buffer) ;
      continue ;
    }
    mwSize dimensions [3] = {image->height, image->width, image->depth} ;
    mxArray * image_array = mxCreateNumericArray(3, dimensions, mxSINGLE_CLASS, mxREAL) ;
    mxSetCell(out[OUT_IMAGES], i, image_array) ;
    memcpy(mxGetData(image_array), image->buffer,
           image->height*image->width*image->depth*sizeof(float)) ;
    if (image->filename) free(image->filename) ;
    if (image->buffer) free(image->buffer) ;
    free(image) ;
  }
}






















