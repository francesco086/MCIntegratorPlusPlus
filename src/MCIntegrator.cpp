#include "mci/MCIntegrator.hpp"
#include "mci/Estimators.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>


//   --- Integrate

void MCI::integrate(const int64_t &Nmc, double * average, double * error, const bool doFindMRT2step, const bool doDecorrelation)
{
    if (Nmc<static_cast<int64_t>(_nblocks)) {
        throw std::invalid_argument("The requested number of MC steps is smaller than the requested number of blocks.");
    }

    const bool fixedBlocks = (_nblocks>0);
    const int64_t stepsPerBlock = fixedBlocks ? Nmc/_nblocks : 1;
    const int64_t trueNmc = fixedBlocks ? stepsPerBlock*_nblocks : Nmc;
    const int64_t ndatax = fixedBlocks ? _nblocks : Nmc;

    if ( _flagpdf )
    {
        //find the optimal mrt2 step
        if (doFindMRT2step) { this->findMRT2Step();
}
        // take care to do the initial decorrelation of the walker
        if (doDecorrelation) { this->initialDecorrelation();
}
    }

    //allocation of the array where the data will be stored
    _datax = new double*[ndatax];
    for (int64_t i=0; i<ndatax; ++i) { *(_datax+i) = new double[_nobsdim]; }

    //sample the observables
    if (_flagobsfile) { _obsfile.open(_pathobsfile);
}
    if (_flagwlkfile) { _wlkfile.open(_pathwlkfile);
}
    _flagMC = true;
    this->sample(trueNmc, true, stepsPerBlock);
    _flagMC = false;
    if (_flagobsfile) { _obsfile.close();
}
    if (_flagwlkfile) { _wlkfile.close();
}

    //reduce block averages
    if (fixedBlocks) {
        const double fac = 1./stepsPerBlock;
        for (int64_t i=0; i<ndatax; ++i) {
            for (int j=0; j<_nobsdim; ++j) {
                *(*(_datax+i)+j) *= fac;
            }
        }
    }

    //estimate average and standard deviation
    if ( _flagpdf && !fixedBlocks)
        {
            mci::MultiDimCorrelatedEstimator(ndatax, _nobsdim, _datax, average, error);
        }
    else
        {
            mci::MultiDimUncorrelatedEstimator(ndatax, _nobsdim, _datax, average, error);
            if (!_flagpdf) {
                for (int i=0; i<_nobsdim; ++i) {
                    *(average+i) *=_vol;
                    *(error+i) *=_vol;
                }
            }
        }

    //deallocation of the data array
    for (int i=0; i<ndatax; ++i){ delete [] *(_datax+i); }
    delete [] _datax;
    _datax = nullptr;
}



//   --- Internal methods


void MCI::storeObservables()
{
    if ( _ridx%_freqobsfile == 0 )
        {
            _obsfile << _ridx;
            for (auto & _ob : _obs) {
                for (int j=0; j<_ob->getNObs(); ++j)
                {
                    _obsfile << "   " << _ob->getObservable(j);
                }
            }
            _obsfile << std::endl;
        }
}


void MCI::storeWalkerPositions()
{
    if ( _ridx%_freqwlkfile == 0 )
        {
            _wlkfile << _ridx;
            for (int j=0; j<_ndim; ++j)
                {
                    _wlkfile << "   " << *(_xold+j) ;
                }
            _wlkfile << std::endl;
        }
}


void MCI::initialDecorrelation()
{
    if (_NdecorrelationSteps < 0) {
        int64_t i;
        //constants
        const int64_t MIN_NMC=100;
        //allocate the data array that will be used
        _datax = new double*[MIN_NMC];
        for (i=0; i<MIN_NMC; ++i) { *(_datax+i) = new double[_nobsdim]; }
        //do a first estimate of the observables
        this->sample(MIN_NMC, true);
        auto * oldestimate = new double[_nobsdim];
        double olderrestim[_nobsdim];
        mci::MultiDimCorrelatedEstimator(MIN_NMC, _nobsdim, _datax, oldestimate, olderrestim);
        //start a loop which will stop when the observables are stabilized
        bool flag_loop=true;
        auto * newestimate = new double[_nobsdim];
        double newerrestim[_nobsdim];
        double * foo;
        while ( flag_loop ) {
            flag_loop = false;
            this->sample(MIN_NMC, true);
            mci::MultiDimCorrelatedEstimator(MIN_NMC, _nobsdim, _datax, newestimate, newerrestim);
            for (i=0; i<_nobsdim; ++i) {
                if ( fabs( *(oldestimate+i) - *(newestimate+i) ) > *(olderrestim+i) + *(newerrestim+i) ) {
                    flag_loop=true;
                }
            }
            foo=oldestimate;
            oldestimate=newestimate;
            newestimate=foo;
        }
        //memory deallocation
        delete [] newestimate;
        delete [] oldestimate;
        for (i=0; i<MIN_NMC; ++i){ delete[] *(_datax+i); }
        delete [] _datax;
        _datax = nullptr;
    }
    else if (_NdecorrelationSteps > 0) {
        this->sample(_NdecorrelationSteps, false);
    }
}


void MCI::sample(const int64_t &npoints, const bool &flagobs, const int64_t &stepsPerBlock)
{
    if (flagobs && stepsPerBlock>0 && npoints%stepsPerBlock!=0) {
        throw std::invalid_argument("If fixed blocking is used, npoints must be a multiple of stepsPerBlock.");
    }

    int i;
    //initialize the running indices
    _ridx=0;
    _bidx=0;

    if (flagobs) {
        // set the data to 0
        for (i=0; i<npoints/stepsPerBlock; ++i) {
            for (int64_t j=0; j<_nobsdim; ++j) {
                *(*(_datax+i)+j) = 0.;
            }
        }
    }

    //initialize the pdf at x
    computeOldSamplingFunction();
    //initialize the observables values at x
    if (flagobs)
        {
            this->computeObservables();
        }
    //reset acceptance and rejection
    this->resetAccRejCounters();
    //first call of the call-back functions
    if (flagobs){
        for (MCICallBackOnAcceptanceInterface * cback : _cback){
            cback->callBackFunction(_xold, true);
        }
    }
    //start the main loop for sampling
    if ( _flagpdf )
        {
            if ( flagobs )
                {
                    for (i=0; i<npoints; ++i)
                        {
                            bool flagacc = this->doStepMRT2();
                            if (flagacc)
                                {
                                    this->computeObservables();
                                    this->saveObservables();
                                } else
                                {
                                    this->saveObservables();
                                }
                            if (_flagMC){
                                if (_flagobsfile) { this->storeObservables();
}
                                if (_flagwlkfile) { this->storeWalkerPositions();
}
                            }
                            _ridx++;
                            _bidx = _ridx / stepsPerBlock;
                        }
                } else
                {
                    for (i=0; i<npoints; ++i)
                        {
                            this->doStepMRT2();
                            _ridx++;
                            _bidx = _ridx / stepsPerBlock;
                        }
                }
        }
    else
        {
            if ( flagobs )
                {
                    for (i=0; i<npoints; ++i)
                        {
                            this->newRandomX();
                            _acc++; // autoaccept move
                            this->computeObservables();
                            this->saveObservables();
                            if (_flagMC){
                                if (_flagobsfile) { this->storeObservables();
}
                                if (_flagwlkfile) { this->storeWalkerPositions();
}
                            }
                            _ridx++;
                            _bidx = _ridx / stepsPerBlock;
                        }
                } else
                {
                    for (i=0; i<npoints; ++i)
                        {
                            this->newRandomX();
                            _acc++; // autoaccept move
                            _ridx++;
                            _bidx = _ridx / stepsPerBlock;
                        }
                }
        }
}


void MCI::findMRT2Step()
{
    int j;
    //constants
    const int MIN_STAT=100;  //minimum statistic: number of M(RT)^2 steps done before deciding if the step must be increased or decreased
    const int MIN_CONS=10;  //minimum consecutive: minimum number of consecutive loops without need of changing mrt2step
    const double TOLERANCE=0.05;  //tolerance: tolerance for the acceptance rate
    const int MAX_NUM_ATTEMPTS=100;  //maximum number of attempts: maximum number of time that the main loop can be executed
    const double SMALLEST_ACCEPTABLE_DOUBLE=1.e-50;

    //initialize index
    int cons_count = 0;  //number of consecutive loops without need of changing mrt2step
    int counter = 0;  //counter of loops
    double fact;
    while ( (cons_count < MIN_CONS && _NfindMRT2steps < 0) || counter < _NfindMRT2steps)
        {
            counter++;
            //do MIN_STAT M(RT)^2 steps
            this->sample(MIN_STAT,false);
            //increase or decrease mrt2step depending on the acceptance rate
            double rate = this->getAcceptanceRate();
            if ( rate > _targetaccrate+TOLERANCE )
                {
                    //need to increase mrt2step
                    cons_count=0;
                    fact = std::min(2.,rate/_targetaccrate);
                    for (j=0; j<_ndim; ++j)
                        {
                            *(_mrt2step+j) *= fact;
                        }
                } else if ( rate < _targetaccrate-TOLERANCE)
                {
                    //need to decrease mrt2step
                    cons_count=0;
                    fact = std::max(0.5, rate/_targetaccrate);
                    for (j=0; j<_ndim; ++j)
                        {
                            *(_mrt2step+j) *= fact;
                        }
                } else
                {
                    //mrt2step was ok
                    cons_count++;
                }
            //check if the loop is running since too long
            if ( counter > MAX_NUM_ATTEMPTS ) { cons_count=MIN_CONS; }
            //mrt2step = Infinity
            for (j=0; j<_ndim; ++j)
                {
                    if ( *(_mrt2step+j) - ( *(*(_irange+j)+1) - *(*(_irange+j)) ) > 0. )
                        {
                            *(_mrt2step+j) = ( *(*(_irange+j)+1) - *(*(_irange+j)) );
                        }
                }
            //mrt2step ~ 0
            for (j=0; j<_ndim; ++j)
                {
                    if ( *(_mrt2step+j) < SMALLEST_ACCEPTABLE_DOUBLE )
                        {
                            *(_mrt2step+j) = SMALLEST_ACCEPTABLE_DOUBLE;
                        }
                }
        }
}


bool MCI::doStepMRT2()
{
    // propose a new position x
    this->computeNewX();

    // find the corresponding sampling function value
    this->computeNewSamplingFunction();

    //determine if the proposed x is accepted or not
    bool flagacc = ( this->computeAcceptance() > _rd(_rgen) );

    //update some values according to the acceptance of the mrt2 step
    if ( flagacc )
        {
            //accepted
            _acc++;
            //update the walker position x
            this->updateX();
            //update the sampling function values pdfx
            this->updateSamplingFunction();
            //if there are some call back functions, invoke them
            for (MCICallBackOnAcceptanceInterface * cback : _cback){
                cback->callBackFunction(_xold, _flagMC);
            }
        } else
        {
            //rejected
            _rej++;
        }

    return flagacc;
}


void MCI::applyPBC(double * v)
{
    for (int i=0; i<_ndim; ++i)
        {
            while ( *(v+i) < *(*(_irange+i)) )
                {
                    *(v+i) += *(*(_irange+i)+1) - *(*(_irange+i)) ;
                }
            while ( *(v+i) > *(*(_irange+i)+1) )
                {
                    *(v+i) -= ( *(*(_irange+i)+1) - *(*(_irange+i)) ) ;
                }
        }
}


void MCI::updateX()
{
    double * foo;
    foo = _xold;
    _xold = _xnew;
    _xnew = foo;
}


void MCI::newRandomX()
{
    //generate a new random x (within the irange)
    for (int i=0; i<_ndim; ++i) {
        *(_xold+i) = *(*(_irange+i)) + ( *(*(_irange+i)+1) - *(*(_irange+i)) ) * _rd(_rgen);
    }
}


void MCI::resetAccRejCounters()
{
    _acc = 0;
    _rej = 0;
}


void MCI::computeNewX()
{
    for (int i=0; i<_ndim; ++i)
        {
            *(_xnew+i) = *(_xold+i) + (*(_mrt2step+i)) * (_rd(_rgen)-0.5);
        }
    applyPBC(_xnew);
}


void MCI::updateSamplingFunction()
{
    for (auto & sf : _pdf)
        {
            sf->newToOld();
        }
}


double MCI::computeAcceptance()
{
    double acceptance=1.;
    for (auto & sf : _pdf)
        {
            acceptance*=sf->getAcceptance();
        }
    return acceptance;
}


void MCI::computeOldSamplingFunction()
{
    for (auto & sf : _pdf)
        {
            sf->computeNewSamplingFunction(_xold);
            sf->newToOld();
        }
}


void MCI::computeNewSamplingFunction()
{
    for (auto & sf : _pdf)
        {
            sf->computeNewSamplingFunction(_xnew);
        }
}


void MCI::computeObservables()
{
    for (auto & _ob : _obs)
        {
            _ob->computeObservables(_xold);
        }
}


void MCI::saveObservables()
{
    int idx=0;
    //save in _datax the observables contained in _obs
    for (auto & _ob : _obs)
        {
            for (int j=0; j<_ob->getNObs(); ++j)
                {
                    _datax[_bidx][idx]+=_ob->getObservable(j);
                    idx++;
                }
        }
}



//   --- Setters


void MCI::storeObservablesOnFile(const char * filepath, const int &freq)
{
    _pathobsfile.assign(filepath);
    _freqobsfile = freq;
    _flagobsfile=true;
}


void MCI::storeWalkerPositionsOnFile(const char * filepath, const int &freq)
{
    _pathwlkfile.assign(filepath);
    _freqwlkfile = freq;
    _flagwlkfile=true;
}


void MCI::clearCallBackOnAcceptance(){
    _cback.clear();
}


void MCI::addCallBackOnAcceptance(MCICallBackOnAcceptanceInterface * cback){
    _cback.push_back(cback);
}


void MCI::clearObservables()
{
    _obs.clear();
    _nobsdim=0;
}


void MCI::addObservable(MCIObservableFunctionInterface * obs)
{
    _obs.push_back(obs);
    _nobsdim+=obs->getNObs();
}


void MCI::clearSamplingFunctions()
{
    _pdf.clear();
    _flagpdf = false;
}


void MCI::addSamplingFunction(MCISamplingFunctionInterface * mcisf)
{
    _pdf.push_back(mcisf);
    _flagpdf = true;
}


void MCI::setTargetAcceptanceRate(const double targetaccrate)
{
    _targetaccrate = targetaccrate;
}


void MCI::setMRT2Step(const double * mrt2step)
{
    for (int i=0; i<_ndim; ++i)
        {
            *(_mrt2step+i) = *(mrt2step+i);
        }
}


void MCI::setX(const double * x)
{
    for (int i=0; i<_ndim; ++i)
        {
            *(_xold+i) = *(x+i);
        }
    applyPBC(_xold);
}


void MCI::setIRange(const double * const * irange)
{
    // Set _irange and apply PBC to the initial walker position _x
    for (int i=0; i<_ndim; ++i)
        {
            *(*(_irange+i)) = *(*(irange+i));
            *(*(_irange+i)+1) = *(*(irange+i)+1);
        }
    applyPBC(_xold);
    // Set the integration volume
    _vol=1.;
    for (int i=0; i<_ndim; ++i)
        {
            _vol = _vol*( *(*(_irange+i)+1) - *(*(_irange+i)) );
        }
}

void MCI::setSeed(const uint_fast64_t seed) // fastest unsigned integer which is at least 64 bit (as expected by rgen)
{
    _rgen.seed(seed);
}


//   --- Constructor and Destructor

MCI::MCI(const int & ndim)
{
    int i;
    // _ndim
    _ndim = ndim;
    // _irange
    _irange = new double*[_ndim];
    for (i=0; i<_ndim; ++i)
        {
            *(_irange+i) = new double[2];
            *(*(_irange+i)) = -std::numeric_limits<double>::max();
            *(*(_irange+i)+1) = std::numeric_limits<double>::max();
        }
    // _vol
    _vol=0.;
    // _x
    _xold = new double[_ndim];
    for (i=0; i<_ndim; ++i)
        {
            _xold[i] = 0.;
        }
    _xnew = new double[_ndim];
    for (i=0; i<_ndim; ++i)
        {
            _xnew[i] = 0.;
        }
    _flagwlkfile=false;
    // _mrt2step
    _mrt2step = new double[_ndim];
    for (i=0; i<_ndim; ++i)
        {
            _mrt2step[i] = INITIAL_STEP;
        }

    // other controls, defaulting to auto behavior
    _NfindMRT2steps = -1;
    _NdecorrelationSteps = -1;
    _nblocks = 16; // defaulting to auto-blocking is not good idea, so we use 16 block default

    // probability density function
    _flagpdf = false;
    // initialize info about observables
    _nobsdim=0;
    _flagobsfile=false;
    // initialize random generator
    _rgen = std::mt19937_64(_rdev());
    _rd = std::uniform_real_distribution<double>(0.,1.);
    //initialize the running indices
    _ridx=0;
    _bidx=0;
    _datax=nullptr;
    // initialize all the other variables
    _targetaccrate=0.5;
    _flagMC=false;
    resetAccRejCounters();
}

MCI::~MCI()
{
    // _irange
    for (int i=0; i<_ndim; ++i){delete[] *(_irange+i);}
    delete[] _irange;
    // _xold and _xnew
    delete [] _xold;
    delete [] _xnew;
    // _mrt2step
    delete [] _mrt2step;
    // vectors
    _pdf.clear();
    _obs.clear();
}
