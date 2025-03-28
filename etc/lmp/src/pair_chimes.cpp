/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing author: Rebecca K. Lindsey (LLNL)
------------------------------------------------------------------------- */


#include "math.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "mpi.h"
#include "atom.h"
#include "force.h"
#include "comm.h"
#include "neighbor.h"
#include "neigh_list.h"
#include "neigh_request.h"
#include "my_page.h"
#include "math_const.h"
#include "math_special.h"
#include "memory.h"
#include "error.h"
#include "pair_chimes.h"
#include "group.h"
#include "update.h"

#include <vector>
#include <iostream>

using namespace LAMMPS_NS;



/*	Functions required by LAMMPS:


settings 	(done)		reads the input script line with arguments defined here
coeff		(done)		set coefficients for one i,j pair type
compute		(done)		workhorse routine that computes pairwise interactions
init_one	(done)		perform initalization for one i,j type pair
init_style 	(done)		initialization specific to this pair style

write_restart			write i,j pair coeffs to restart file
read_restart			read i,j pair coeffs from restart file
write_restart_settings		write global settings to restart file
read_restart_settings		read global settings from restart file
single				force and energy fo a single pairwise interaction between two atoms
*/


PairCHIMES::PairCHIMES(LAMMPS *lmp) : Pair(lmp)
{
	restartinfo = 0;

	int me = comm->me;
	MPI_Comm_rank(world,&me);
	
	chimes_calculator.init(me);
	
	// 2, 3, and 4-body vars for chimesFF access

	dr     .resize(CHDIM);
	dr_3b  .resize(3*CHDIM); 
	dr_4b  .resize(6*CHDIM);
	
	dist_3b.resize(3);			       
	dist_4b.resize(6);			       

	// CHDIM is the number of spatial dimensions (usually 3).
	force_2b.resize(2*CHDIM);
	force_3b.resize(3*CHDIM) ;
	force_4b.resize(4*CHDIM) ;
	
	typ_idxs_2b.resize(2);
	typ_idxs_3b.resize(3);
	typ_idxs_4b.resize(4);
	
	// Vars for neighlist construction
	
	tmp_3mer.resize(3);
	tmp_4mer.resize(4);   
	
	if (chimes_calculator.rank == 0)
	{ 
		std::cout << std::endl;
		std::cout << "************************* WARNING (pair_style chimesFF) ************************" << std::endl;
		std::cout << "Assuming n-body interactions have longer cutoffs than all (n+1)-body interactions" << std::endl;
		std::cout << "************************* WARNING (pair_style chimesFF) ************************" << std::endl;
		std::cout << std::endl;
	}
		
}

PairCHIMES::~PairCHIMES()
{
	if (allocated) 
	{   	    
	    memory->destroy(setflag);
	    memory->destroy(cutsq);
	}
}	

void PairCHIMES::settings(int narg, char **arg)
{
	if (narg != 0) 
		error -> all(FLERR,"Illegal pair_style command. Expects no arguments beyond pair_style name.");

	return;	
}

void PairCHIMES::coeff(int narg, char **arg)
{
	// Expect: pair_coeff * * <parameter file name>

	if (narg != 3) 
		error -> all(FLERR,"Illegal pair_style command. Expects \"pair_coeff * * <parameter file name>\" ");
	
	chimesFF_paramfile = arg[2]; 
	
	chimes_calculator.read_parameters(chimesFF_paramfile);

	set_chimes_type();
    //chimes_calculator.set_atomtypes(chimes_type);
    chimes_calculator.build_pair_int_trip_map() ; 
    chimes_calculator.build_pair_int_quad_map() ;

	// Set special LAMMPS flags/cutoffs
	
	if (!allocated)
		allocate();
		
	vector<vector<double> > cutoff_2b;
	chimes_calculator.get_cutoff_2B(cutoff_2b);

	for(int i=1; i<=atom->ntypes; i++)
	{
		for(int j=i; j<=atom->ntypes; j++)
		{
			setflag[i][j] = 1;
			setflag[j][i] = 1;
			
			cutsq[i][j]  = cutoff_2b[ chimes_calculator.get_atom_pair_index( chimes_type[i-1]*chimes_calculator.natmtyps + chimes_type[j-1] ) ][1];
			cutsq[i][j] *= cutsq[i][j];
			
			if (i!=j)
			{
				cutsq[j][i]  = cutoff_2b[ chimes_calculator.get_atom_pair_index( chimes_type[j-1]*chimes_calculator.natmtyps + chimes_type[i-1]) ][1];
				cutsq[j][i] *= cutsq[j][i];
			}			
		}
	}

	maxcut_3b = chimes_calculator.max_cutoff_3B();
	maxcut_4b = chimes_calculator.max_cutoff_4B();
}

void PairCHIMES::allocate()
{
	allocated = 1;
	
	memory->create(setflag,atom->ntypes+1,atom->ntypes+1,"pair:setflag");
	
	for(int i=1; i<=atom->ntypes; i++)
		for(int j=i; j<=atom->ntypes; j++)
			setflag[i][j] = 0;

	memory->create(cutsq,atom->ntypes+1,atom->ntypes+1,"pair:cutsq");
}	

void PairCHIMES::init_style()
{
	if (atom -> tag_enable == 0)
		error -> all(FLERR,"Pair style ChIMES requires atom IDs");
	
	if (force->newton_pair == 0)
		error->all(FLERR,"Pair style ChIMES requires newton pair on");	
	
	// Set up neighbor lists... borrowing this from pair_airebo:
	// need a full neighbor list, including neighbors of ghosts

	// int irequest = neighbor->request(this,instance_me);
	// neighbor->requests[irequest]->half = 0;
	// neighbor->requests[irequest]->full = 1;
	// neighbor->requests[irequest]->ghost = 1;
        neighbor->add_request(this, NeighConst::REQ_FULL | NeighConst::REQ_GHOST);
}

double PairCHIMES::init_one(int i, int j)
{
	// Sets the cutoff for each pair interaction.
	// The maximum of the returned values are used to set outer cutoff for neighbor lists
	// WARNING: This means linking won't work properly if 2-b interactions do not have larger cutoffs than all other
	// higher bodied interactions!!
	
	if (setflag[i][j] == 0) 
		error->all(FLERR,"All pair coeffs are not set");
	
	return sqrt(cutsq[i][j]);
}

inline double PairCHIMES::get_dist(int i, int j, double *dr)
{
	double 	**x    = atom -> x;	// Access to system coordinates

	dr[0] = x[j][0] - x[i][0];  
	dr[1] = x[j][1] - x[i][1];
	dr[2] = x[j][2] - x[i][2];

	return sqrt(dr[0]*dr[0] + dr[1]*dr[1] + dr[2]*dr[2]);
}

inline double PairCHIMES::get_dist(int i, int j)
{
	double dummy_dr[3] ;

	return get_dist(i,j, dummy_dr);
}

void PairCHIMES::build_mb_neighlists()
{

	if ( (chimes_calculator.poly_orders[1] == 0) &&  (chimes_calculator.poly_orders[2] == 0))
		return;

	// List gets built based on atoms owned by calling proc. 
	
	neighborlist_3mers.clear();
	neighborlist_4mers.clear();
	
	int i,j,k,l,inum,jnum,knum,lnum, ii, jj, kk, ll;		// Local iterator vars
	int *ilist,*jlist,*klist,*llist, *numneigh,**firstneigh;	// Local neighborlist vars
	tagint 	*tag   = atom -> tag;					// Access to global atom indices
	int     itag, jtag, ktag, ltag;					// holds tags	
	double 	**x    = atom -> x;					// Access to system coordinates
	
	double maxcut_3b_padded = maxcut_3b + neighbor-> skin;
	double maxcut_4b_padded = maxcut_4b + neighbor-> skin;
	
	double dist_ij, dist_ik, dist_il, dist_jk, dist_jl, dist_kl;
	
	////////////////////////////////////////
	// Access to neighbor list vars
	////////////////////////////////////////

	inum       = list -> inum; 		// length of the list
	ilist      = list -> ilist; 		// list of i atoms for which neighbor list exists
	numneigh   = list -> numneigh;		// length of each of the ilist neighbor lists
	firstneigh = list -> firstneigh;	// point to the list of neighbors of i	
	
	for (ii = 0; ii < inum; ii++)	// Loop over real atoms (ai)	
	{
		i     = ilist[ii];		
		itag  = tag[i];			
		jlist = firstneigh[i];		
		jnum  = numneigh[i];	

		for (jj = 0; jj < jnum; jj++)	
		{
			j     = jlist[jj];	
			jtag  = tag[j];		
			j    &= NEIGHMASK;

			if (j == i)
				continue;
			if (jtag < itag) 
				continue;				
				
			// Check ij distance

			dist_ij = get_dist(i,j);
			
			if ( (dist_ij >= maxcut_3b_padded) && (dist_ij >= maxcut_4b_padded) )
				continue;

			klist = firstneigh[i];	// ChIMES assumes all atoms must be within cutoff of eachother for a valid interaction	
			knum  = numneigh[i];	
			
			for (kk = 0; kk < knum; kk++)	
			{
				k     = klist[kk];	
				ktag  = tag[k];		
				k    &= NEIGHMASK;

				if ( (k==i) || (k==j) )
					continue;
				if ( (ktag < itag) || (ktag < jtag) )
					continue;						
							
 	 			// Check ik distance			

				dist_ik = get_dist(i,k);

				if ( (dist_ik >= maxcut_3b_padded) && (dist_ik >= maxcut_4b_padded) )
					continue;
					
				// Check jk distance			

				dist_jk = get_dist(j,k);
				
				if( (dist_ij < maxcut_3b_padded) &&  (dist_ik < maxcut_3b_padded) && (dist_jk < maxcut_3b_padded) )
				{
					// If we're here and valid_3mer == true, then add the triplet to the chimes neigh list        

					tmp_3mer[0] = i;
					tmp_3mer[1] = j;
					tmp_3mer[2] = k;
				
					neighborlist_3mers.push_back(tmp_3mer);
				}
									
				if ((dist_ij >= maxcut_4b_padded) || (dist_ik >= maxcut_4b_padded) || (dist_jk >= maxcut_4b_padded) )	
					continue;					
				
				// Now decide if we should continue on to 4-body neighbor list construction

				if (chimes_calculator.poly_orders[2] == 0)
					continue;

				llist = firstneigh[i];	
				lnum  = numneigh[i];	
					
				for (ll = 0; ll < lnum; ll++)	
				{
					l     = llist[ll];	
					ltag  = tag[l];		
					l    &= NEIGHMASK;
					
					if ( (l==i) || (l==j) || (l==k))
						continue;
					if ((ltag < itag) ||(ltag < jtag)||(ltag < ktag)) 
						continue;
											
					// Check il distance			

					dist_il = get_dist(i,l); 

					if (dist_il >= maxcut_4b_padded)
						continue;	

					// Check jl distance			
	
					dist_jl = get_dist(j,l);
	
					if (dist_jl >= maxcut_4b_padded)
						continue;
								
					// Check kl distance			

					dist_kl = get_dist(k,l);
	
					if (dist_kl >= maxcut_4b_padded)
						continue;
		
					// If we're here and valid_4mer == true, then add the quadruplet to the chimes neigh list
					
					tmp_4mer[0] = i;
					tmp_4mer[1] = j;
					tmp_4mer[2] = k;
					tmp_4mer[3] = l;
					
					neighborlist_4mers.push_back(tmp_4mer);
					
				}				
			}
		}
	}
}

void PairCHIMES::compute(int eflag, int vflag)
{
	// Vars for access to chimesFF compute_XB functions
	
	std::vector  <double>  stensor(6);	// pointers to system stress tensor


	// Hold the "partial" stress tensor from each of the three pairs in a 3B interaction
	// Or six pairs in a 4B interaction
			
	std::vector  <double>  stensor0(6);
	std::vector  <double>  stensor1(6);	
	std::vector  <double>  stensor2(6);
	std::vector  <double>  stensor3(6);
	std::vector  <double>  stensor4(6);
	std::vector  <double>  stensor5(6);
    
	// Temp vars to hold chimes output for passing to ev_tally function
	
    std::vector<double>         fscalar(6);
    std::vector<double>         force_scalar(6);
    std::vector<double>         tmp_dist(1);
    std::vector<double>         tmp_dr(6);
    int atmidxlst[6][2];
	
	// General LAMMPS compute vars
	
	int 	i,j,k,l,inum,jnum, ii, jj;		// Local iterator vars
	int 	*ilist,*jlist, *numneigh,**firstneigh;	// Local neighborlist vars
	int     idx;

	double 	**x    = atom -> x;		// Access to system coordinates
	double 	**f    = atom -> f;		// Access to system forces
	
	int 	*type  = atom -> type;		// Acces to system atom types (countng starts from 1, chimesFF class expects counting from 0!)
	tagint 	*tag   = atom -> tag;		// Access to global atom indices (sort of like "parent" indices)
	int     itag, jtag, ktag, ltag;		// holds tags
	int 	nlocal = atom -> nlocal;	// Number of real atoms owned by current process .. used used to assure force assignments aren't duplicated
	int 	newton_pair = force -> newton_pair;	// Should f_j be automatically set to -f_i (true) or manually calculated (false)
	double  energy;				// pair energy 

	int me = comm->me;
	MPI_Comm_rank(world,&me);	
	
	// Set up vars controlling if energy/pressure (virial) contributions are computed

	ev_init(eflag, vflag);

	////////////////////////////////////////
	// Access to (2-body) neighbor list vars
	////////////////////////////////////////

	inum       = list -> inum; 		// length of the list
	ilist      = list -> ilist; 		// list of i atoms for which neighbor list exists
	numneigh   = list -> numneigh;		// length of each of the ilist neighbor lists
	firstneigh = list -> firstneigh;	// point to the list of neighbors of i


    chimes2BTmp chimes_2btmp(chimes_calculator.poly_orders[0]) ;
    chimes3BTmp chimes_3btmp(chimes_calculator.poly_orders[1]) ;
    chimes4BTmp chimes_4btmp(chimes_calculator.poly_orders[2]) ;      
	
	// Build the ChIMES many-body neighbor lists.. only do so when LAMMPS neighborlist has been updated
	
	if ( neighbor->ago == 0)
	{
		// if (chimes_calculator.rank == 0){
			// std::cout << "Updating chimesFF neighbor lists..." << '\n';
		// }
			
		build_mb_neighlists();		
		// if (chimes_calculator.rank == 0)
		// {
			// std::cout << "	Rank " << me << " 3-body list size: " << neighborlist_3mers.size() << '\n';
			// std::cout << "	Rank " << me << " 4-body list size: " << neighborlist_4mers.size() << '\n';
			// std::cout << "	...update complete" << '\n';
		// }
	}

	////////////////////////////////////////
	// Compute 1- and 2-body interactions
	////////////////////////////////////////
	
	for (ii = 0; ii < inum; ii++)		// Loop over the atoms owned by the current process
	{
		i     = ilist[ii];				// Index of the current atom
		itag  = tag[i];					// Get i's global atom index (sort of like its "parent")

		jlist = firstneigh[i];			// Neighborlist for atom i
		jnum  = numneigh[i];			// Number of neighbors of atom i
		
		// First, get the single-atom energy contribution
		
		energy = 0.0;
		
		chimes_calculator.compute_1B(type[i]-1, energy);
		
		if(evflag)
			ev_tally_mb(0, atmidxlst, energy, fscalar, tmp_dist, tmp_dr);

		// Now move on to two-body force, stress, and energy
		
		for (jj = 0; jj < jnum; jj++)			// Loop over neighbors of i
		{
			j     = jlist[jj];			// Index of the jj atom
			jtag  = tag[j];				// Get j's global atom index (sort of like its "parent")
			j    &= NEIGHMASK;			// Strip possible extra bits of j
				
			
			if (jtag <= itag) // only allow calculation for j<i, since we've requested a full neighbor list
				continue;
				
			// Get distance using ghost atoms... don't need MIC since we're using ghost atoms

			dist = get_dist(i,j,&dr[0]);
			
			typ_idxs_2b[0] = chimes_type[type[i]-1];		// Type (index) of the current atom... subtract 1 to account for chimesFF vs LAMMPS numbering convention
			typ_idxs_2b[1] = chimes_type[type[j]-1];

			// Using std::fill for maximum efficiency.
			std::fill(force_2b.begin(), force_2b.end(), 0.0) ;

			// Do the same for stress tensors
			std::fill(stensor.begin(), stensor.end(), 0.0) ;

			energy = 0.0;	
		
			chimes_calculator.compute_2B( dist, dr, typ_idxs_2b, force_2b, stensor, energy, chimes_2btmp, fscalar[0]);
			
			for (idx=0; idx<3; idx++)
			{
				f[i][idx] += force_2b[0*CHDIM+idx] ;
				f[j][idx] += force_2b[1*CHDIM+idx] ;
			}

			// "Save"/tally up the energy and stresses to the global virial/energy data objects (see pair.cpp ~ line 1000)
			// Compute pressure, (in contrast to chimes_md) AFTER penalty has been added		
			
			if(vflag_atom)
			{
			    atmidxlst[0][0] = i;
			    atmidxlst[0][1] = j;
			}

			tmp_dist    [0] = dist;
			
			if (evflag)
				ev_tally_mb(1, atmidxlst, energy, stensor, tmp_dist, dr);         
		}
	}

	if (chimes_calculator.poly_orders[1] > 0)
	{
		////////////////////////////////////////
		// Compute 3-body interactions
		////////////////////////////////////////

		for (ii = 0; ii < neighborlist_3mers.size(); ii++)		
		{
			i     = neighborlist_3mers[ii][0];
			j     = neighborlist_3mers[ii][1];
			k     = neighborlist_3mers[ii][2];

			dist_3b[0] = get_dist(i,j,&dr_3b[0*CHDIM]);
			dist_3b[1] = get_dist(i,k,&dr_3b[1*CHDIM]);
			dist_3b[2] = get_dist(j,k,&dr_3b[2*CHDIM]);

			typ_idxs_3b[0] = chimes_type[type[i]-1];
			typ_idxs_3b[1] = chimes_type[type[j]-1];
			typ_idxs_3b[2] = chimes_type[type[k]-1];

			std::fill(force_3b.begin(), force_3b.end(), 0.0) ;
			std::fill(stensor.begin(), stensor.end(), 0.0) ;

			std::fill(stensor0.begin(), stensor0.end(), 0.0) ;
			std::fill(stensor1.begin(), stensor1.end(), 0.0) ;
			std::fill(stensor2.begin(), stensor2.end(), 0.0) ;

				
			energy = 0.0 ;
			
			chimes_calculator.compute_3B( dist_3b, dr_3b, typ_idxs_3b, force_3b, stensor, stensor0, stensor1, stensor2, energy, chimes_3btmp, fscalar);

			std::vector<std::vector<double>> combined = {stensor0, stensor1, stensor2};

			// fscalar2 = force_scalar;

			for (idx=0; idx<3; idx++)
			{
				f[i][idx] += force_3b[0*CHDIM+idx] ;
				f[j][idx] += force_3b[1*CHDIM+idx] ;
				f[k][idx] += force_3b[2*CHDIM+idx] ;
			}

            if (vflag_atom)
            {
			    atmidxlst[0][0] = i;
			    atmidxlst[0][1] = j;
			    atmidxlst[1][0] = i;
			    atmidxlst[1][1] = k;
			    atmidxlst[2][0] = j;
			    atmidxlst[2][1] = k;
            }
			
			if (evflag)
				ev_tally_mb2(3, atmidxlst, energy, combined, dist_3b, dr_3b);		            
		}		
	}

	if (chimes_calculator.poly_orders[2] > 0)
	{
		////////////////////////////////////////
		// Compute 4-body interactions
		////////////////////////////////////////
		
		for (ii = 0; ii < neighborlist_4mers.size(); ii++)		
		{
			i     = neighborlist_4mers[ii][0];
			j     = neighborlist_4mers[ii][1];
			k     = neighborlist_4mers[ii][2];
			l     = neighborlist_4mers[ii][3];			
			
			dist_4b[0] = get_dist(i,j,&dr_4b[0*CHDIM]);				      
			dist_4b[1] = get_dist(i,k,&dr_4b[1*CHDIM]);
			dist_4b[2] = get_dist(i,l,&dr_4b[2*CHDIM]);
			dist_4b[3] = get_dist(j,k,&dr_4b[3*CHDIM]);
			dist_4b[4] = get_dist(j,l,&dr_4b[4*CHDIM]);
			dist_4b[5] = get_dist(k,l,&dr_4b[5*CHDIM]);

			typ_idxs_4b[0] = chimes_type[type[i]-1];
			typ_idxs_4b[1] = chimes_type[type[j]-1];
			typ_idxs_4b[2] = chimes_type[type[k]-1];
			typ_idxs_4b[3] = chimes_type[type[l]-1];

			std::fill(force_4b.begin(), force_4b.end(), 0.0) ;
			std::fill(stensor.begin(), stensor.end(), 0.0) ;

			std::fill(stensor0.begin(), stensor0.end(), 0.0) ;
			std::fill(stensor1.begin(), stensor1.end(), 0.0) ;
			std::fill(stensor2.begin(), stensor2.end(), 0.0) ;
			std::fill(stensor3.begin(), stensor3.end(), 0.0) ;
			std::fill(stensor4.begin(), stensor4.end(), 0.0) ;
			std::fill(stensor5.begin(), stensor5.end(), 0.0) ;

			energy = 0.0 ;	
			
			chimes_calculator.compute_4B( dist_4b, dr_4b, typ_idxs_4b, force_4b, stensor, stensor0, stensor1, stensor2, stensor3, stensor4, stensor5, energy, chimes_4btmp, fscalar);

			std::vector<std::vector<double>> combined2 = {stensor0, stensor1, stensor2, stensor3, stensor4, stensor5};

			for (idx=0; idx<3; idx++)
			{
				f[i][idx] += force_4b[0*CHDIM+idx] ;
				f[j][idx] += force_4b[1*CHDIM+idx] ;
				f[k][idx] += force_4b[2*CHDIM+idx] ;
				f[l][idx] += force_4b[3*CHDIM+idx] ;
			}
			
            if (vflag_atom)
            {
			    atmidxlst[0][0] = i;
			    atmidxlst[0][1] = j;
			    atmidxlst[1][0] = i;
			    atmidxlst[1][1] = k;
			    atmidxlst[2][0] = i;
			    atmidxlst[2][1] = l;
			    atmidxlst[3][0] = j;
			    atmidxlst[3][1] = k;
			    atmidxlst[4][0] = j;
			    atmidxlst[4][1] = l;
			    atmidxlst[5][0] = k;
			    atmidxlst[5][1] = l;
            }
			
			if (evflag)
				ev_tally_mb2(6, atmidxlst, energy, combined2, dist_4b, dr_4b);	
            
		}
	}

if (vflag_fdotr) 
        virial_fdotr_compute();

	return;
}

void PairCHIMES::set_chimes_type()
{
	int nmatches = 0;

	for (int i=1; i<= atom->ntypes; i++) 						// Lammps indexing starts at 1
	{
		for (int j=0; j<chimes_calculator.natmtyps; j++) 			// ChIMES indexing starts at 0
		{
			if (abs(atom->mass[i] - chimes_calculator.masses[j]) < 1e-3)	// Masses should match to at least 3 decimal places
			{
				chimes_type.push_back(j);
				nmatches++;
			}
		}	}

	
	if (nmatches < atom->ntypes )
	{
		std::cout << "ERROR: LAMMPS coordinate file has " << atom->ntypes << " atom type masses" << std::endl; 
		std::cout << "       but only found " << nmatches << " matches with the ChIMES parameter file." << std::endl;
		exit(0);
	} 
}
							
void PairCHIMES::write_restart(){}			
void PairCHIMES::read_restart(){}				
void PairCHIMES::write_restart_settings(){}		
void PairCHIMES::read_restart_settings(){}	
void PairCHIMES::single(){}					
