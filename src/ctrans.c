/* 
 
 pyspec.ccd.ctrans 
 (c) 2010 Stuart Wilkins <stuwilkins@mac.com>
 
 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 
 $Id$
 
 */

#include <Python.h>
#include <numpy/arrayobject.h>
#include <stdlib.h>
#include <math.h>
#include <pthread.h>
#include "ctrans.h"

static PyObject* ccdToQ(PyObject *self, PyObject *args, PyObject *kwargs){
  static char *kwlist[] = { "angles", "mode", "ccd_size", "ccd_pixsize", 
			    "ccd_cen", "ccd_bin", "dist", "wavelength", 
			    "UBinv", NULL };
  PyObject *angles, *_angles, *_ubinv, *ubinv;
  PyObject *qOut = NULL;
  CCD ccd;
  npy_intp dims[2];
  npy_intp nimages;
  int i, j, t, stride;
  int ndelgam;
  int mode;

  _float lambda;

  _float *anglesp;
  _float *qOutp;
  _float *ubinvp;
  _float UBI[3][3];

  pthread_t thread[NTHREADS];
  int iret[NTHREADS];
  imageThreadData threadData[NTHREADS];

  if(!PyArg_ParseTupleAndKeywords(args, kwargs, "Oi(ii)(dd)(ii)(ii)ddO", kwlist,
				  &_angles,
				  &mode,
				  &ccd.xSize, &ccd.ySize,
				  &ccd.xPixSize, &ccd.yPixSize, 
				  &ccd.xCen, &ccd.yCen,
				  &ccd.xBin, &ccd.yBin,
				  &ccd.dist,
				  &lambda,
				  &_ubinv)){
    return NULL;
  }

  angles = PyArray_FROMANY(_angles, NPY_DOUBLE, 0, 0, NPY_IN_ARRAY);
  if(!angles){
    return NULL;
  }
  
  ubinv = PyArray_FROMANY(_ubinv, NPY_DOUBLE, 0, 0, NPY_IN_ARRAY);
  if(!ubinv){
    return NULL;
  }

  ubinvp = (_float *)PyArray_DATA(ubinv);
  for(i=0;i<3;i++){
    UBI[i][0] = -1.0 * ubinvp[2];
    UBI[i][1] = ubinvp[1];
    UBI[i][2] = ubinvp[0];
    ubinvp+=3;
  }
  
  
  nimages = PyArray_DIM(angles, 0);
  ndelgam = ccd.xSize * ccd.ySize;

  dims[0] = nimages * ndelgam;
  dims[1] = 4;
  qOut = PyArray_SimpleNew(2, dims, NPY_DOUBLE);
  if(!qOut){
    return NULL;
  }

  anglesp = (_float *)PyArray_DATA(angles);
  qOutp = (_float *)PyArray_DATA(qOut);
  
  stride = nimages / NTHREADS;
  for(t=0;t<NTHREADS;t++){
    // Setup threads
    // Allocate memory for delta/gamma pairs
    
    threadData[t].ccd = &ccd;
    threadData[t].anglesp = anglesp;
    threadData[t].qOutp = qOutp;
    threadData[t].ndelgam = ndelgam;
    threadData[t].lambda = lambda;
    threadData[t].mode = mode;
    threadData[t].imstart = stride * t;
    for(i=0;i<3;i++){
      for(j=0;j<3;j++){
	threadData[t].UBI[j][i] = UBI[j][i];
      }
    }
    if(t == (NTHREADS - 1)){
      threadData[t].imend = nimages;
    } else {
      threadData[t].imend = stride * (t + 1);
    }
    iret[t] = pthread_create( &thread[t], NULL, 
			      processImageThread, 
			      (void*) &threadData[t]);
    
    anglesp += (6 * stride);
    qOutp += (ndelgam * 4 * stride);
  }

  for(t=0;t<NTHREADS;t++){
    if(pthread_join(thread[t], NULL)){
      fprintf(stderr, "ERROR : Cannot join thread %d", t);
    }
  }
  
  return Py_BuildValue("N", qOut);
}

void *processImageThread(void* ptr){
  imageThreadData *data;
  int i;
  _float *delgam;
  data = (imageThreadData*) ptr;
  delgam = (_float*)malloc(data->ndelgam * sizeof(_float) * 2);
  if(!delgam){
    fprintf(stderr, "MALLOC ERROR\n");
    pthread_exit(NULL);
  }
  fprintf(stderr, "From %d To %d delgam = %p\n", data->imstart, data->imend, delgam);
  for(i=data->imstart;i<data->imend;i++){
    // For each image process
    calcDeltaGamma(delgam, data->ccd, data->anglesp[0], data->anglesp[5]);
    calcQTheta(delgam, data->anglesp[1], data->anglesp[4], data->qOutp, 
	       data->ndelgam, data->lambda);
    if(data->mode > 1){
      calcQPhiFromQTheta(data->qOutp, data->ndelgam, 
			 data->anglesp[2], data->anglesp[3]);
    }
    if(data->mode == 4){
      calcHKLFromQPhi(data->qOutp, data->ndelgam, data->UBI);
    }
    data->anglesp+=6;
    data->qOutp+=(data->ndelgam * 4); 
  }
  free(delgam);
  pthread_exit(NULL);
}

int calcQTheta(_float* diffAngles, _float theta, _float mu, _float *qTheta, _int n, _float lambda){
  // Calculate Q in the Theta frame
  // angles -> Six cicle detector angles [delta gamma]
  // theta  -> Theta value at this detector setting
  // mu     -> Mu value at this detector setting
  // qTheta -> Q Values
  // n      -> Number of values to convert
  _int i;
  _float *angles;
  _float *qt;
  _float kl;
  _float del, gam;

  angles = diffAngles;
  qt = qTheta;
  kl = 2 * M_PI / lambda;
  for(i=0;i<n;i++){
    del = *(angles++);
    gam = *(angles++);
    *qt = (-1.0 * sin(gam) * kl) - (sin(mu) * kl);
    //fprintf(stderr, " %lf", *qt);
    qt++;
    *qt = (cos(del - theta) * cos(gam) * kl) - (cos(theta) * cos(mu) * kl);
    //fprintf(stderr, " %lf", *qt);
    qt++;
    *qt = (sin(del - theta) * cos(gam) * kl) + (sin(theta) * cos(mu) * kl);
    //fprintf(stderr, " %lf\n", *qt);
    qt++;
    qt++;
  }
  
  return true;
}

int calcQPhiFromQTheta(_float *qTheta, _int n, _float chi, _float phi){
  _float r[3][3];

  r[0][0] = cos(chi);
  r[0][1] = 0.0;
  r[0][2] = -1.0 * sin(chi);
  r[1][0] = sin(phi) * sin(chi);
  r[1][1] = cos(phi);
  r[1][2] = sin(phi) * cos(chi);
  r[2][0] = cos(phi) * sin(chi);
  r[2][1] = -1.0 * sin(phi);
  r[2][2] = cos(phi) * cos(chi);

  matmulti(qTheta, n, r, 1);
  
  return true;
}

int calcHKLFromQPhi(_float *qPhi, _int n, _float mat[][3]){
  matmulti(qPhi, n, mat, 1);
  return true;
}

int matmulti(_float *val, int n, _float mat[][3], int skip){
  _float *v;
  _float qp[3];
  int i,j,k;

  v = val;

  for(i=0;i<n;i++){
    for(k=0;k<3;k++){
      qp[k] = 0.0;
      for(j=0;j<3;j++){
	qp[k] += mat[k][j] * v[j];
      }
    }
    for(k=0;k<3;k++){
      v[k] = qp[k];
    }
    v += 3;
    v += skip;
  }

  return true;
}

int calcDeltaGamma(_float *delgam, CCD *ccd, _float delCen, _float gamCen){
  // Calculate Delta Gamma Values for CCD
  int i,j;
  _float *delgamp;
  _float xPix, yPix;

  xPix = (_float)ccd->xPixSize / ccd->dist;
  yPix = (_float)ccd->yPixSize / ccd->dist;
  delgamp = delgam;

  //fprintf(stderr, "xBin = %d\n", ccd->xBin);
  //fprintf(stderr, "xPixSize = %lf\n", ccd->xPixSize);
  //fprintf(stderr, "dist = %lf\n", ccd->dist);
  //fprintf(stderr, "xPix = %e, yPix %e\n", xPix, yPix); 
  //fprintf(stderr, "DelCen = %lf, GamCen = %lf\n", delCen, gamCen);
  for(j=0;j<ccd->ySize;j++){
    for(i=0;i<ccd->xSize;i++){
      //fprintf(stderr, "DelCen = %lf\n", delCen);
      *delgamp = delCen - atan( (j - ccd->yCen) * yPix);
      //fprintf(stderr, "Delta = %lf", *delgamp);
      delgamp++;
      *delgamp = gamCen - atan( (i - ccd->xCen) * xPix); 
      //fprintf(stderr, " Gamma = %lf\n", *delgamp);
      delgamp++;
    }
  }

  return true;
} 

static PyObject* gridder_3D(PyObject *self, PyObject *args, PyObject *kwargs){
	PyObject *gridout = NULL, *Nout = NULL;
	PyObject *gridI = NULL;
	PyObject *_I;
	
	static char *kwlist[] = { "data", "xrange", "yrange", "zrange", "norm", NULL };

	npy_intp data_size;
	npy_intp dims[3];
	
	double grid_start[3];
	double grid_stop[3];
	int grid_nsteps[3];
	int norm_data = 0;

	unsigned long n_outside;
	
	if(!PyArg_ParseTupleAndKeywords(args, kwargs, "O(ddd)(ddd)(iii)|i", kwlist,
					&_I, 
					&grid_start[0], &grid_start[1], &grid_start[2],
					&grid_stop[0], &grid_stop[1], &grid_stop[2],
					&grid_nsteps[0], &grid_nsteps[1], &grid_nsteps[2],
					&norm_data)){
	  return NULL;
	}	
	
	gridI = PyArray_FROMANY(_I, NPY_DOUBLE, 0, 0, NPY_IN_ARRAY);
	if(!gridI){
		return NULL;
	}
	
	data_size = PyArray_DIM(gridI, 0);
	
	dims[0] = grid_nsteps[0];
	dims[1] = grid_nsteps[1];
	dims[2] = grid_nsteps[2];
	gridout = PyArray_ZEROS(3, dims, NPY_DOUBLE, 0);
	if(!gridout){
		return NULL;
	}
	Nout = PyArray_ZEROS(3, dims, NPY_ULONG, 0);
	if(!Nout){
		return NULL;
	}
	
	n_outside = c_grid3d(PyArray_DATA(gridout), PyArray_DATA(Nout), 
			     PyArray_DATA(gridI),
			     grid_start, grid_stop, data_size, grid_nsteps, norm_data);
	
	return Py_BuildValue("NNl", gridout, Nout, n_outside); 
}

unsigned long c_grid3d(double *dout, unsigned long *nout, double *data, 
		       double *grid_start, double *grid_stop, int max_data, 
		       int *n_grid, int norm_data){
	int i;
	unsigned long *nout_ptr;
	double *dout_ptr;
	double *data_ptr;
	
	double pos_double[3];
	double grid_len[3], grid_step[3];
	int grid_pos[3];
	int pos;
	unsigned long n_outside = 0;
	
	fprintf(stderr, "Gridding in 3D : grid pts = %i x %i x %i, data pts = %i\n", 
		n_grid[0], n_grid[1], n_grid[2], max_data);
	
	dout_ptr = dout;
	nout_ptr = nout;
	
	data_ptr = data;
	for(i = 0;i < 3; i++){
		grid_len[i] = grid_stop[i] - grid_start[i];
		grid_step[i] = grid_len[i] / (n_grid[i]);
	}
	
	for(i = 0; i < max_data ; i++){
		pos_double[0] = (*data_ptr - grid_start[0]) / grid_len[0];
		data_ptr++;
		pos_double[1] = (*data_ptr - grid_start[1]) / grid_len[1];
		data_ptr++;
		pos_double[2] = (*data_ptr - grid_start[2]) / grid_len[2];
		if((pos_double[0] >= 0) && (pos_double[0] < 1) && 
		   (pos_double[1] >= 0) && (pos_double[1] < 1) &&
		   (pos_double[2] >= 0) && (pos_double[2] < 1)){
			data_ptr++;	
			grid_pos[0] = (int)(pos_double[0] * n_grid[0]);
			grid_pos[1] = (int)(pos_double[1] * n_grid[1]);
			grid_pos[2] = (int)(pos_double[2] * n_grid[2]);
			pos =  grid_pos[0] * (n_grid[1] * n_grid[2]);
			pos += grid_pos[1] * n_grid[2];
			pos += grid_pos[2];
			dout[pos] = dout[pos] + *data_ptr;
			nout[pos] = nout[pos] + 1;
			data_ptr++;
		} else {
		  //fprintf(stderr, "Data point (%lf,%lf,%lf) is out of range\n", pos_double[0], pos_double[1], pos_double[2]);
		  n_outside++;
		  data_ptr+=2;
		}
	}

	if(norm_data){
	  for(i = 0; i < (n_grid[0] * n_grid[1] * n_grid[2]); i++){
	    if(nout[i] > 0){
	      dout[i] = dout[i] / nout[i];
	    } else {
	      dout[i] = 0.0;
	    }
	  }
	}
	
	return n_outside;
}

PyMODINIT_FUNC initctrans(void)  {
	(void) Py_InitModule3("ctrans", _ctransMethods, _ctransDoc);
	import_array();  // Must be present for NumPy.  Called first after above line.
}
