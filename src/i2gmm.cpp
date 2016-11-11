#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "DebugUtils.h"
#include "DataSet.h"
#include "util.h"
#include "Dish.h"
#include "Restaurant.h"
#include "Customer.h"
#include <thread>  
#include <iostream>
#include <algorithm>


using namespace std;


int MAX_SWEEP=1500;
int BURNIN=1000;
int SAMPLE=(MAX_SWEEP-BURNIN)/10; // Default value is 10 sample + 1 post burnin
char* result_dir = "./";

// Variables
double kep;


class TotalLikelihood : public Task
{
public:
	atomic<int> taskid;
	int ntable;
	int nchunks;
	vector<Customer>& customers;
	atomic<double> totalsum;
	TotalLikelihood(vector<Customer>& customers,int nchunks) : nchunks(nchunks), customers(customers){
	}
	void reset() {
		totalsum = 0;
		taskid = 0;
	}
	void run(int id) {
		// Use thread specific buffer
		SETUP_ID()
		int taskid = this->taskid++; // Oth thread is the main process
		auto range = trange(n, nchunks, taskid); // 2xNumber of Threads chunks			
		double logsum = 0;
		for (auto i = range[0]; i< range[1]; i++) // Operates on its own chunk
		{
			logsum += customers[i].table->dist.likelihood(customers[i].data);
		}
		totalsum = totalsum + logsum;
	}
};


void updateTableDish(list<Dish>& franchise,vector<Restaurant>& Restaurants)
{
	for (auto dit = franchise.begin(); dit != franchise.end(); dit++)
		dit->reset();
	for (int i = 0; i < Restaurants.size(); i++) // Update stats
	{
		//Each table
		for (auto tit = Restaurants[i].tables.begin(); tit != Restaurants[i].tables.end(); tit++)
		{
			tit->dishp->addCluster(*tit);
		}
	}

	for (auto dit = franchise.begin(); dit != franchise.end(); dit++)
		dit->calculateDist();
	for (int i = 0; i < Restaurants.size(); i++)
	{
		// Each table
		for (auto tit = Restaurants[i].tables.begin(); tit != Restaurants[i].tables.end(); tit++)
		{
			tit->calculateDist();
		}
	}
}


PILL_DEBUG
int main(int argc,char** argv)
{
	debugMode(1);
	char* datafile = argv[1];
	char* priorfile = argv[2];
	char* configfile = argv[3];
	string ss(result_dir);
	ofstream nsampleslog(ss.append("nsamples.log"));
	srand(time(NULL));
	// Default values , 1000 , 100  , out
	if (argc>4)
		MAX_SWEEP = atoi(argv[4]);
	if (argc>5)
		BURNIN = atoi(argv[5]);
	if (argc > 6)
		result_dir = argv[6];
	SAMPLE = (MAX_SWEEP-BURNIN)/10; // Default value
	if (argc>7)
		SAMPLE = atoi(argv[7]);
	
	step();
	// Computing buffer

	printf("Reading...\n");
	nthd = thread::hardware_concurrency() * 2;
	DataSet ds(datafile,priorfile,configfile);

	kep = kappa*kappa1/(kappa + kappa1);
	
	precomputeGammaLn(2*(n+d)+1+200*d);  // With 0.5 increments
	Vector priormean(d); 
	Matrix priorvariance(d,d);
	priorvariance = Psi*((kep+1)/((kep)*(m-d+2)));
	priormean = mu0;
	
	Stut stt(priormean,priorvariance,m-d+2); 
	Vector loglik0;
	
	Dish emptyDish(stt);
	vector<Restaurant> Restaurants;
	vector<Restaurant> beststate;
	vector<vector<Restaurant>::iterator> Restaurantit; // Hash table
	list<Dish> franchise;
	list<Dish> bestdishes;
	Matrix     sampledLabels((MAX_SWEEP-BURNIN)/SAMPLE + 1,n);
	Table besttable;
	Customer bestcustomer;
	int i,j,k;
	
	// INITIALIZATION
	printf("Number of Cores : %d\n",thread::hardware_concurrency());



	step();
	loglik0		= emptyDish.dist.likelihood(ds.data);
	// customers.reserve(ds.n);
	step();
	
	// One cluster for each object initially
	franchise.push_back(Dish(d));
	list<Dish>::iterator firstDish = franchise.begin();
	i=0;
	Restaurants.reserve(200);
	Restaurants.resize(1);
	Restaurants[i].Restaurantid = 0;
	Table t(firstDish); // First dish
	Restaurants[i].addTable(t); 
	
	// Create customers
	vector<Customer> allcust;
	for(i=0;i<n;i++)
	{
		Restaurants[0].tables.begin()->addInitPoint(ds.data(i));
		Restaurants[0].customers.emplace_back(ds.data(i),loglik0[i],Restaurants[0].tables.begin(),i+1);
	}
	allcust = Restaurants[0].customers;
	i=0;
	Restaurants[i].tables.front().calculateCov();
	firstDish->addCluster(Restaurants[i].tables.front());

	step();
	firstDish->calculateDist();
	Restaurants[i].tables.front().calculateDist();

	//END INITIALIZATION

	// GIBBS SAMPLER


	// NUMBER OF THREADS
	// =================
	ThreadPool tpool(nthd);
	// ==================



	list<Dish>::iterator dit,ditc;
	list<Table>::iterator tit;
	vector<Customer>::iterator cit;
	list<vector<Customer>::iterator> intable;
	list<vector<Customer>::iterator>::iterator points;

	double newdishprob,maxdishprob,sumprob,val,logprob,gibbs_score,best_score;
	int kal=1,id;
	gibbs_score = -INFINITY;
	best_score = -INFINITY;

	Vector score(MAX_SWEEP+1);
	vector<Restaurant> dishrestaurants;
	TotalLikelihood tl(allcust, nthd);
	for (int num_sweep = 0;num_sweep <= MAX_SWEEP ; num_sweep++)
	{


		if (hypersample  )
		{
			// Update tables
			for (i = 0; i < Restaurants.size(); i++)
			{
				for (auto& customer : Restaurants[i].customers)
				{
					allcust[customer.id - 1].table = customer.table;
				}
			}

			Vector logprob(50);
			int idx;
			k = 0;
			for (kappa = 0.025; kappa < 5; kappa += 0.1)
			{
				kappa1 = 10 * kappa;
				updateTableDish(franchise, Restaurants);
				tl.reset();
				for (i = 0; i<tl.nchunks; i++)
					tpool.submit(tl);
				tpool.waitAll(); // Wait for finishing all jobs
				logprob[k] = tl.totalsum / n;

				k++;
				//cout << logprob << endl;
			}
			idx = sampleFromLog(logprob);
			kappa = idx*0.1 + 0.025;
			kappa1 = 10 * kappa;
			//cout << kappa << endl;
			updateTableDish(franchise, Restaurants);

			//k = 0;
			//for (kappa1 = 0.025; kappa1 < 2; kappa1 += 0.1)
			//{
			//	updateTableDish(franchise, Restaurants);
			//	tl.reset();
			//	for (i = 0; i<tl.nchunks; i++)
			//		tpool.submit(tl);
			//	tpool.waitAll(); // Wait for finishing all jobs
			//					 //cout << tl.totalsum << ":";
			//	logprob[k] = tl.totalsum / n;

			//	k++;
			//	//cout << logprob << endl;
			//}
			////logprob.print();
			//idx = sampleFromLog(logprob);
			//kappa1 = idx*0.1 + 0.025;
			//updateTableDish(franchise, Restaurants);
			////cout << kappa1 << endl;

			//Matrix oldPsi = Psi;
			//k = 0;
			//for (m = d + 2; m < (d + 2 + 100 * d); m += 2*d)
			//{
			//	Psi = eye(d)*(m - d - 1);
			//	updateTableDish(franchise, Restaurants);
			//	tl.reset();
			//	for (i = 0; i < tl.nchunks; i++)
			//		tpool.submit(tl);
			//	tpool.waitAll(); // Wait for finishing all jobs
			//	logprob[k] = tl.totalsum / n;
			//	k++;
			//	//cout << logprob << endl;
			//}
			////logprob.print();
			//idx = sampleFromLog(logprob);
			//m = d + 2 + (idx*2*d);
			////cout << m << endl;
			//Psi = eye(d)*(m - d - 1);
			//updateTableDish(franchise, Restaurants);
			////Psi
			///*
			//k = 0;
			/*Psi = eye(d)*(m - d - 1.);
			for (dit = franchise.begin(); dit != franchise.end(); dit++,k++)
			Psi += dit->sampleScatter;
			Psi /= ((n+m-d-1.)/((m - d - 1.)));*/
			//logprob.resize(20
			//Psi = eye(d)*(m - d - 1.);
			//Psi = oldPsi;
			for (int dim = 0; dim < d; dim++) {
				k = 0;
				for (double ps = 0.05; ps < 2.05; ps += 0.04)
				{
					Psi.data[dim*d + dim] = ps*(m - d - 1);
					updateTableDish(franchise, Restaurants);
					tl.reset();
					for (i = 0; i<tl.nchunks; i++)
						tpool.submit(tl);
					tpool.waitAll(); // Wait for finishing all jobs
					logprob[k] = tl.totalsum / n;
					k++;
				}
				idx = sampleFromLog(logprob);
				Psi.data[dim*d + dim] = (0.05 + (idx)*0.04) *(m - d - 1);
				updateTableDish(franchise, Restaurants);
			}
			//logprob.resize(5);
			//priormean = mu0;
			//for (int dim = 0; dim < d; dim++) {
			//	k = 0;
			//	for (double add = -0.25; add < 0.25; add += 0.01)
			//	{
			//		mu0.data[dim] = priormean[dim] + add;
			//		updateTableDish(franchise, Restaurants);
			//		tl.reset();
			//		for (i = 0; i<tl.nchunks; i++)
			//			tpool.submit(tl);
			//		tpool.waitAll(); // Wait for finishing all jobs
			//		logprob[k] = tl.totalsum / n;
			//		k++;
			//	}
			//	idx = sampleFromLog(logprob);
			//	mu0.data[dim] = idx*0.01 - 0.25;
			//	updateTableDish(franchise, Restaurants);
			//}
			//(Psi/(m-d-1)).print();
			//priormean = mu0;
			kep = kappa*kappa1 / (kappa + kappa1);
			priorvariance = Psi*((kep + 1) / ((kep)*(m - d + 2)));
			loglik0 = Stut(priormean, priorvariance, m - d + 2).likelihood(ds.data);

		}







		//Create restaurants for each dish
		dishrestaurants.resize(0); // Remove all
		dishrestaurants.resize(franchise.size());
		for (i=0,dit=franchise.begin();dit!=franchise.end();dit++,i++)
		{
			id = dit->dishid = i;
			dishrestaurants[id].Restaurantid = id;
			dishrestaurants[id].customers.reserve(dit->nsamples);
			dishrestaurants[id].likelihood=0;
		}

		// Copy tables
		for (i=0;i<Restaurants.size();i++) // Old Restaurants
			for (auto& table : Restaurants[i].tables)
			{
				id = table.dishp->dishid;
				dishrestaurants[id].tables.push_back(table);
				table.copy = (--dishrestaurants[id].tables.end()); //Last item copied
			}

		// Add customers
		for (i=0;i<Restaurants.size();i++)
		{
			for (auto& customer : Restaurants[i].customers)
			{
				allcust[ customer.id - 1].table = customer.table;
			}

		}

		for (i=0;i<n;i++)
		{
			id = allcust[i].table->dishp->dishid;
			allcust[i].table = allcust[i].table->copy;
			dishrestaurants[id].customers.push_back(allcust[i]);
		}

		// Submit Jobs
		// 1st Loop
		//

		for (dit=franchise.begin();dit!=franchise.end();dit++)
		{
			dit->copy = dit;
		}
		Restaurants.resize(0);
		Restaurants = dishrestaurants;
		random_shuffle(Restaurants.begin(),Restaurants.end());

		printf("Iter %d nDish %d nRests %d Score %.1f\n",num_sweep,franchise.size(),Restaurants.size(),gibbs_score);	
			
		for (i=0;i<Restaurants.size();i++)
			tpool.submit(Restaurants[i]);	
		tpool.waitAll(); // Wait for finishing all jobs


		for(dit=franchise.begin();dit!=franchise.end();dit++) // For each dish
		{
			dit->reset(); // To start recalculation
		}

		for (i=0;i<Restaurants.size();i++)
		{
			for(tit=Restaurants[i].tables.begin();tit!=Restaurants[i].tables.end();tit++)
			{
				tit->dishp->addCluster(*tit);
			}
		}

		for(dit=franchise.begin();dit!=franchise.end();dit++) // For each dish
		{
			dit->calculateDist();
		}
		

		// 3rd Loop
		for(dit=franchise.begin();dit!=franchise.end();dit++)
		{
			dit->logprob = 0;
			nsampleslog << dit->nsamples << " ";
		}
		nsampleslog << endl;

		for (i=0,kal=1;i<Restaurants.size();i++)
		{
			// Each table
			for(tit=Restaurants[i].tables.begin();tit!=Restaurants[i].tables.end();tit++,kal++)
			{
				tit->dishp->removeCluster(*tit);
				
				if (tit->dishp->ntables==0) // Remove dish 
					franchise.erase(tit->dishp);
				else
					tit->dishp->calculateDist();
				if (intable.size()>0)
				{
					int sss = intable.size();
					intable.clear();
				}
				

				// Create list of customers
				for(cit=Restaurants[i].customers.begin();cit!=Restaurants[i].customers.end();cit++)
				{
					if (cit->table == tit)
						intable.push_back(cit);
				}

				newdishprob = tit->npoints * log(gamma);//+ log(3/(franchise.size() + 1.0 )); // A possion factor added centered on 3 clusters
				for(points=intable.begin();points!=intable.end();points++)
					newdishprob += (*points)->loglik0;

				maxdishprob = newdishprob;

				for(dit=franchise.begin();dit!=franchise.end();dit++) 
				{
					logprob=0;
					for(points=intable.begin();points!=intable.end();points++)
					{
						// Here is computationally time consuming Under 4 for loops matrix division !!!!!!
						logprob+=dit->dist.likelihood((*points)->data);
					}
					dit->logprob = logprob + tit->npoints * log(dit->ntables); //Prior


					if (maxdishprob<dit->logprob)
						maxdishprob = dit->logprob;
				}

				sumprob=0;
				for(dit=franchise.begin();dit!=franchise.end();dit++) 
				{
					dit->logprob = exp(dit->logprob - maxdishprob);
					sumprob += dit->logprob;
				}

				sumprob += exp(newdishprob - maxdishprob);

				double rrr = urand();
				val = rrr*sumprob;

				for(dit=franchise.begin();dit!=franchise.end();dit++) 
				{
					if ((dit->logprob)>=val)
						break;
					else
						val -= dit->logprob;
				}

				if (dit==franchise.end()) // Create new dish
				{
					franchise.emplace_back(d);
					dit = franchise.end();
					dit--; // Point to actual dish
				}

				tit->dishp = dit;
				dit->addCluster(*tit);
				dit->calculateDist();
			}
		}

		// 4th loop 
		for (i = 0; i < Restaurants.size(); i++)
		{
			// Each table
			for (tit = Restaurants[i].tables.begin(); tit != Restaurants[i].tables.end(); tit++)
			{
				tit->calculateDist();
			}
		}


		// Calculate Gibbs Score
		gibbs_score = 0;
		for (i=0;i<Restaurants.size();i++)
			gibbs_score += Restaurants[i].likelihood;
		score[num_sweep] = gibbs_score;
		if ( (best_score < gibbs_score && num_sweep >= BURNIN) || num_sweep == 0)
		{
			best_score = gibbs_score ;
			bestdishes = franchise;
			for (dit=franchise.begin(),ditc=bestdishes.begin();dit!=franchise.end();dit++,ditc++)
			{
				dit->copy = ditc ;
			}
			beststate = Restaurants;
		}


		if  (((num_sweep-BURNIN)%SAMPLE)==0 && num_sweep >= BURNIN)
		{
			for (dit=franchise.begin(),i=0;dit!=franchise.end();dit++,i++)
				dit->dishid = i;
			int error=0;
			
			// Update tables
			for (i=0;i<Restaurants.size();i++)
			{
			for (auto& customer : Restaurants[i].customers)
			{
				allcust[ customer.id - 1].table = customer.table;
			}
			}


			for(i=0;i<n;i++)
			{
				sampledLabels((num_sweep-BURNIN)/SAMPLE)[allcust[i].id-1] = allcust[i].table->dishp->dishid;
				if (allcust[i].table->dishp->dishid<0 || allcust[i].table->dishp->dishid>franchise.size())
				{
					error = 1;
					printf("Something is wrong in C code : dishid %d table %d customer %d\n",allcust[i].table->dishp->dishid,allcust[i].table->tableid,allcust[i].id-1);
				}
			}
			if (error)
				pause();
		}
		
		flush(cout);
		// 2nd Loop
	}
	step();


	franchise = bestdishes;
	for (dit=franchise.begin(),ditc=bestdishes.begin();dit!=franchise.end();dit++,ditc++)
				ditc->copy = dit;
	Restaurants = beststate;
	

	string s(result_dir);
	ofstream dishfile( s.append("Dish.dish"),ios::out | ios::binary); // Be careful result_dir should include '\'
	s.assign(result_dir);
	ofstream restfile( s.append("Restaurant.rest"),ios::out | ios::binary);
	s.assign(result_dir);
	ofstream likefile( s.append("Likelihood.matrix"),ios::out | ios::binary);
	
	s.assign(result_dir);
	ofstream labelfile( s.append("Labels.matrix"),ios::out | ios::binary);

	int ndish = franchise.size();
	int nrest = Restaurants.size();
	dishfile.write((char*)& ndish,sizeof(int));
	restfile.write((char*)& nrest,sizeof(int));

	for(i=0,dit=franchise.begin();dit!=franchise.end();i++,dit++) 
	{
		dit->dishid = i+1;
		dishfile <<  *dit;
	}
	dishfile.close();


	for (i=0;i<nrest;i++)
	{
		restfile << Restaurants[i];
	}

	restfile.close();


	likefile << score;
	likefile.close();

	labelfile << sampledLabels;
	labelfile.close();

	nsampleslog.close();
}
