/* -----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under 
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------ */

#include "math.h"
#include "string.h"
#include "improper_hybrid.h"
#include "atom.h"
#include "neighbor.h"
#include "domain.h"
#include "comm.h"
#include "force.h"
#include "memory.h"
#include "error.h"

using namespace LAMMPS_NS;

#define EXTRA 1000

/* ---------------------------------------------------------------------- */

ImproperHybrid::ImproperHybrid(LAMMPS *lmp) : Improper(lmp)
{
  nstyles = 0;
}

/* ---------------------------------------------------------------------- */

ImproperHybrid::~ImproperHybrid()
{
  if (nstyles) {
    for (int i = 0; i < nstyles; i++) delete styles[i];
    delete [] styles;
    for (int i = 0; i < nstyles; i++) delete [] keywords[i];
    delete [] keywords;
  }

  if (allocated) {
    memory->sfree(setflag);
    memory->sfree(map);
    delete [] nimproperlist;
    delete [] maximproper;
    for (int i = 0; i < nstyles; i++)
      memory->destroy_2d_int_array(improperlist[i]);
    delete [] improperlist;
  }
}

/* ---------------------------------------------------------------------- */

void ImproperHybrid::compute(int eflag, int vflag)
{
  int i,m,n;

  // save ptrs to original improperlist

  int nimproperlist_orig = neighbor->nimproperlist;
  int **improperlist_orig = neighbor->improperlist;

  // if this is re-neighbor step, create sub-style improperlists
  // nimproperlist[] = length of each sub-style list
  // realloc sub-style improperlist if necessary
  // load sub-style improperlist with 5 values from original improperlist

  if (neighbor->ago == 0) {
    for (m = 0; m < nstyles; m++) nimproperlist[m] = 0;
    for (i = 0; i < nimproperlist_orig; i++)
      nimproperlist[map[improperlist_orig[i][4]]]++;
    for (m = 0; m < nstyles; m++) {
      if (nimproperlist[m] > maximproper[m]) {
	memory->destroy_2d_int_array(improperlist[m]);
	maximproper[m] = nimproperlist[m] + EXTRA;
	improperlist[m] = (int **)
	  memory->create_2d_int_array(maximproper[m],5,
				      "improper_hybrid:improperlist");
      }
      nimproperlist[m] = 0;
    }
    for (i = 0; i < nimproperlist_orig; i++) {
      m = map[improperlist_orig[i][4]];
      n = nimproperlist[m];
      improperlist[m][n][0] = improperlist_orig[i][0];
      improperlist[m][n][1] = improperlist_orig[i][1];
      improperlist[m][n][2] = improperlist_orig[i][2];
      improperlist[m][n][3] = improperlist_orig[i][3];
      improperlist[m][n][4] = improperlist_orig[i][4];
      nimproperlist[m]++;
    }
  }
  
  // call each sub-style's compute function
  // must set neighbor->improperlist to sub-style improperlist before call
  // accumulate sub-style energy,virial in hybrid's energy,virial

  energy = 0.0;
  if (vflag) for (n = 0; n < 6; n++) virial[n] = 0.0;

  for (m = 0; m < nstyles; m++) {
    if (styles[m] == NULL) continue;
    neighbor->nimproperlist = nimproperlist[m];
    neighbor->improperlist = improperlist[m];
    styles[m]->compute(eflag,vflag);
    if (eflag) energy += styles[m]->energy;
    if (vflag) for (n = 0; n < 6; n++) virial[n] += styles[m]->virial[n];
  }

  // restore ptrs to original improperlist

  neighbor->nimproperlist = nimproperlist_orig;
  neighbor->improperlist = improperlist_orig;
}

/* ---------------------------------------------------------------------- */

void ImproperHybrid::allocate()
{
  allocated = 1;
  int n = atom->nimpropertypes;

  map = (int *) memory->smalloc((n+1)*sizeof(int),"improper:map");
  setflag = (int *) memory->smalloc((n+1)*sizeof(int),"improper:setflag");
  for (int i = 1; i <= n; i++) setflag[i] = 0;

  nimproperlist = new int[nstyles];
  maximproper = new int[nstyles];
  improperlist = new int**[nstyles];
  for (int m = 0; m < nstyles; m++) maximproper[m] = 0;
  for (int m = 0; m < nstyles; m++) improperlist[m] = NULL;
}

/* ----------------------------------------------------------------------
   create one improper style for each arg in list
------------------------------------------------------------------------- */

void ImproperHybrid::settings(int narg, char **arg)
{
  nstyles = narg;
  styles = new Improper*[nstyles];
  keywords = new char*[nstyles];

  for (int m = 0; m < nstyles; m++) {
    for (int i = 0; i < m; i++)
      if (strcmp(arg[m],arg[i]) == 0) 
	error->all("Improper style hybrid cannot use same improper style twice");
    if (strcmp(arg[m],"hybrid") == 0) 
      error->all("Improper style hybrid cannot have hybrid as an argument");
    styles[m] = force->new_improper(arg[m]);
    keywords[m] = new char[strlen(arg[m])+1];
    strcpy(keywords[m],arg[m]);
  }
}

/* ----------------------------------------------------------------------
   set coeffs for one type
---------------------------------------------------------------------- */

void ImproperHybrid::coeff(int which, int narg, char **arg)
{
  if (!allocated) allocate();

  int ilo,ihi;
  force->bounds(arg[0],atom->nimpropertypes,ilo,ihi);

  // 2nd arg = improper style name (harmonic, etc)

  int m;
  for (m = 0; m < nstyles; m++)
    if (strcmp(arg[1],keywords[m]) == 0) break;
  if (m == nstyles) error->all("Improper coeff for hybrid has invalid style");

  // set low-level coefficients for each impropertype
  // replace 2nd arg with i, call coeff() with no 1st arg
  // if sub-style is NULL for "none", still set setflag

  for (int i = ilo; i <= ihi; i++) {
    sprintf(arg[1],"%d",i);
    map[i] = m;
    if (styles[m]) styles[m]->coeff(which,narg-1,&arg[1]);
    if (styles[m] == NULL) setflag[i] = 1;
    else setflag[i] = styles[m]->setflag[i];
  }
}

/* ----------------------------------------------------------------------
   proc 0 writes to restart file
------------------------------------------------------------------------- */

void ImproperHybrid::write_restart(FILE *fp)
{
  fwrite(&nstyles,sizeof(int),1,fp);

  int n;
  for (int m = 0; m < nstyles; m++) {
    n = strlen(keywords[m]) + 1;
    fwrite(&n,sizeof(int),1,fp);
    fwrite(keywords[m],sizeof(char),n,fp);
  }
}

/* ----------------------------------------------------------------------
   proc 0 reads from restart file, bcasts
------------------------------------------------------------------------- */

void ImproperHybrid::read_restart(FILE *fp)
{
  allocate();

  int me = comm->me;
  if (me == 0) fread(&nstyles,sizeof(int),1,fp);
  MPI_Bcast(&nstyles,1,MPI_INT,0,world);
  styles = new Improper*[nstyles];
  keywords = new char*[nstyles];
  
  int n;
  for (int m = 0; m < nstyles; m++) {
    if (me == 0) fread(&n,sizeof(int),1,fp);
    MPI_Bcast(&n,1,MPI_INT,0,world);
    keywords[m] = new char[n];
    if (me == 0) fread(keywords[m],sizeof(char),n,fp);
    MPI_Bcast(keywords[m],n,MPI_CHAR,0,world);
    styles[m] = force->new_improper(keywords[m]);
  }
}

/* ----------------------------------------------------------------------
   memory usage
------------------------------------------------------------------------- */

double ImproperHybrid::memory_usage()
{
  double bytes = 0.0;
  for (int m = 0; m < nstyles; m++) bytes += maximproper[m]*5 * sizeof(int);
  for (int m = 0; m < nstyles; m++) 
    if (styles[m]) bytes += styles[m]->memory_usage();
  return bytes;
}
