#include "mci/Estimators.hpp"
#include "mci/BlockAccumulator.hpp"
#include "mci/FullAccumulator.hpp"
#include "mci/SimpleAccumulator.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <random>

#include "../common/TestMCIFunctions.hpp"


using namespace std;
using namespace mci;

void reportAvgErr1D(const string &label, double avg, double err)
{
    cout << "- " << label << endl;
    cout << "     avg = " << avg << "     error = " << err << endl << endl;
}

void reportAvgErrND(const string &label, int ndim, double avg[], double err[])
{
    cout << "- " << label << endl;
    for (int i = 0; i < ndim; ++i) {
        cout << "     avg" << i << " = " << avg[i] << "     error" << i << " = " << err[i] << endl;
    }
    cout << endl;
}

void arrayAvgND(int N1, int N2, const double in[] /*N1*N2*/, double out[] /*N2*/)
{   // calculate average of length N2
    std::fill(out, out + N2, 0.);
    for (int i = 0; i < N1; ++i) {
        for (int j = 0; j < N2; ++j) {
            out[j] += in[i*N2 + j];
        }
    }
    for (int i = 0; i < N2; ++i) { out[i] /= N1; }
}

void arrayErrND(int N1, int N2, const double in[] /*N1*N2*/, double out[] /*N2*/)
{   // estimate standard deviations of length N2, assuming uncorrelated samples according to normal distribution
    double avgs[N2];
    arrayAvgND(N1, N2, in, avgs); // compute averages first
    std::fill(out, out + N2, 0.);
    for (int i = 0; i < N1; ++i) {
        for (int j = 0; j < N2; ++j) {
            out[j] += pow(avgs[j]-in[i*N2 + j], 2);
        }
    }
    for (int i = 0; i < N2; ++i) { out[i] = sqrt(out[i] / (N1 - 1.5)); } // 1.5 is a decent bias correction value for normal distribution
}

void assertArraysEqual(int ndim, const double arr1[], const double arr2[], double tol = 0.)
{
    if (tol > 0.) {
        for (int i = 0; i < ndim; ++i) { assert(fabs(arr1[i] - arr2[i]) < tol); }
    }
    else {
        for (int i = 0; i < ndim; ++i) { assert(arr1[i] == arr2[i]); } // the check above may fail in this case
    }
}


void assertAccuAveragesEqual(const AccumulatorInterface &accu1, const AccumulatorInterface &accu2, double tol = 0.)
{   // check that averages of contained data are equal within tol
    const int nobs = accu1.getNObs();
    assert(nobs == accu2.getNObs());

    double avg1[nobs], avg2[nobs];
    arrayAvgND(accu1.getNStore(), nobs, accu1.getData(), avg1);
    arrayAvgND(accu2.getNStore(), nobs, accu2.getData(), avg2);
    assertArraysEqual(nobs, avg1, avg2, tol);
}

void assertAccuResetted(const AccumulatorInterface &accu)
{   // check that accu is in clean reset state (allocated/deallocated doesn't matter)
    assert(accu.getStepIndex() == 0);
    assert(accu.isClean());
    assert(!accu.isFinalized());
    for (int i = 0; i < accu.getNData(); ++i) { assert(accu.getData()[i] == 0.); }
}

void assertAccuDeallocated(const AccumulatorInterface &accu)
{   // check that the accu is in proper deallocated state
    assert(!accu.isAllocated());
    assert(accu.getNSteps() == 0);
    assert(accu.getNAccu() == 0);
    assert(accu.getNStore() == 0);
    assert(accu.getNData() == 0);

    assertAccuResetted(accu);
}

void assertAccuAllocated(const AccumulatorInterface &accu, int Nmc)
{   // check that the accu is in allocated state (not necessarily reset state)
    assert(accu.isAllocated());
    assert(accu.getNSteps() == Nmc);
    assert(accu.getNAccu() > 0);
    assert(accu.getNStore() > 0);
    assert(accu.getNData() > 0);
    assert(accu.getNData() == accu.getNStore()*accu.getNObs());
}

void assertAccuFinalized(const AccumulatorInterface &accu, int Nmc)
{   // check that the accu is properly finalized
    assert(accu.isAllocated());
    assert(!accu.isClean());
    assert(accu.isFinalized());
    assert(accu.getStepIndex() == Nmc);
}


void accumulateData(AccumulatorInterface &accu, int Nmc, int ndim, const double datax[],
                    const bool datacc[], const int nchanged[], const int changedIdx[])
{   // simulated MC observable accumulation
    WalkerState wlk(ndim, true);
    for (int i = 0; i < Nmc; ++i) {
        std::copy(datax + i*ndim, datax + (i + 1)*ndim, wlk.xnew);
        wlk.nchanged = nchanged[i];
        std::copy(changedIdx + i*ndim, changedIdx + (i + 1)*ndim, wlk.changedIdx);
        wlk.accepted = datacc[i];
        /*
        std::cout << "Step " << i << ":" << std::endl;
        std::cout << "accepted: " << wlk.accepted << ", nchanged: " << wlk.nchanged << std::endl;
        std::cout << "changedIdx: ";
        for (int j=0; j<ndim; ++j) { std::cout << wlk.changedIdx[j] << " "; }
        std::cout << std::endl << "xnew: ";
        for (int j=0; j<ndim; ++j) {
            std::cout << wlk.xnew[j] << " ";
        }
        std::cout << std::endl;
        */
        accu.accumulate(wlk);
    }
    accu.finalize();
}

void checkAccumulator(AccumulatorInterface &accu, int Nmc, int ndim, const double datax[],
                      const bool datacc[], const int nchanged[], const int changedIdx[],
                      double tol /* tolerance for avg */, bool verbose = false /* to enable printout */)
{
    // we expect walker-dim == obs-dim
    assert(accu.getNObs() == ndim);
    assert(accu.getNDim() == ndim);

    // verify that the accumulator is uninitialized
    assertAccuDeallocated(accu);

    // now allocate
    accu.allocate(Nmc);
    assertAccuAllocated(accu, Nmc); // allocated
    assertAccuResetted(accu); // but still clean

    // accumulate the data in pseudo MC loop
    double storedData[accu.getNData()]; // to store away obs data
    accumulateData(accu, Nmc, ndim, datax, datacc, nchanged, changedIdx);
    assertAccuFinalized(accu, Nmc);

    // copy the stored data
    const double * const dataptr = accu.getData(); // we acquire a read-only pointer to data
    std::copy(dataptr, dataptr + accu.getNData(), storedData);

    // now do the same after reset
    accu.reset();
    assertAccuResetted(accu);
    accumulateData(accu, Nmc, ndim, datax, datacc, nchanged, changedIdx);
    assertArraysEqual(accu.getNData(), storedData, accu.getData()); // check that we get the same result

    // now do the same after reallocation
    accu.deallocate();
    assertAccuDeallocated(accu);
    accu.allocate(Nmc);
    assertAccuAllocated(accu, Nmc);
    accu.allocate(Nmc); // do it twice on purpose
    accumulateData(accu, Nmc, ndim, datax, datacc, nchanged, changedIdx);
    assertArraysEqual(accu.getNData(), storedData, accu.getData()); // check that we get the same result

    // finally check that average calculated from the data in
    // the accumulator matches the reference average within tol
    double refAvg[ndim];
    arrayAvgND(Nmc, ndim, datax, refAvg);

    if (accu.getNStore() > 1) {
        double avg[ndim];
        arrayAvgND(accu.getNStore(), ndim, accu.getData(), avg);
        for (int i = 0; i < ndim; ++i) {
            assert(fabs(avg[i] - refAvg[i]) < tol);
            if (verbose) {
                cout << "avg" << i << " " << avg[i] << " refAvg" << i << " " << refAvg[i] << endl;
            }
        }
    }
    else {
        for (int i = 0; i < ndim; ++i) {
            assert(fabs(accu.getData()[i] - refAvg[i]) < tol);
            if (verbose) {
                cout << "avg" << i << " " << accu.getData()[i] << " refAvg" << i << " " << refAvg[i] << endl;
            }
        }
    }
}

int main()
{
    using namespace std;

    bool verbose = false;
    //verbose = true; // uncomment for output

    const double SMALL = 0.01;
    const double TINY = 0.0005;
    const double EXTRA_TINY = 0.00000001; // 1e-8

    const int Nmc = 32768; // use a power of 2, so we can test MJBlocker
    const int nd = 2;
    const int ndata = Nmc*nd;

    const int nblocks = 2048; // blocks to use for fixed-block estimator

    // generate random walk
    double xND[ndata];
    bool accepted[Nmc]; // tells us which steps are new ones
    int nchanged[Nmc]; // tells us how many indices changed
    int changedIdx[ndata]; // tells us which indices changed
    srand(1337); // seed standard random engine
    TestWalk<WalkPDF::GAUSS> testWalk(Nmc, nd, 2., 0.5); // 2-particle walk in 1-dim gauss orbital
    testWalk.generateWalk(xND, accepted, nchanged, changedIdx);
    if (verbose) { cout << testWalk.getAcceptanceRate() << endl; }

    // calculate reference averages and uncorrelated SD estimation
    double refAvg[nd], refErr[nd];
    arrayAvgND(Nmc, nd, xND, refAvg);
    arrayErrND(Nmc, nd, xND, refErr);

    if (verbose) {
        cout << "Reference Average: " << endl << "avg =";
        for (double avg : refAvg) { cout << " " << avg; }
        cout << endl << endl;
        cout << "Uncorrelated Mean Error: " << endl << "err =";
        for (double err : refErr) { cout << " " << err/sqrt(Nmc); }
        cout << endl << endl;
        cout << "Uncorrelated Sample Error: " << endl << "SD =";
        for (double err : refErr) { cout << " " << err; }
        cout << endl << endl;
    }


    // --- check 1D estimators ---
    if (verbose) { cout << endl << "1-dimensional versions of Estimators:" << endl << endl; }

    // helper input for 1D
    double x1D[Nmc];

    for (int i = 0; i < nd; ++i) {
        // copy subdata into continous array
        for (int j = 0; j < Nmc; ++j) { x1D[j] = xND[j*nd + i]; }

        // perform check for correct averages
        double avg1D = 0., err1D = 0.;

        mci::OneDimUncorrelatedEstimator(Nmc, x1D, avg1D, err1D);
        if (verbose) { reportAvgErr1D("UncorrelatedEstimator()", avg1D, err1D); }
        assert(fabs(avg1D - refAvg[i]) < EXTRA_TINY); // these should be virtually identical

        mci::OneDimBlockEstimator(Nmc, x1D, nblocks, avg1D, err1D);
        if (verbose) { reportAvgErr1D("BlockEstimator()", avg1D, err1D); }
        assert(fabs(avg1D - refAvg[i]) < EXTRA_TINY); // these should be virtually identical

        mci::OneDimFCBlockerEstimator(Nmc, x1D, avg1D, err1D);
        if (verbose) { reportAvgErr1D("FCBlockerEstimator()", avg1D, err1D); }
        assert(fabs(avg1D - refAvg[i]) < TINY); // this difference should be TINY

        // the following is currently true with the selected seed
        // and should be quite robust under (valid) change
        assert(fabs(avg1D - refAvg[i]) < 3*err1D);

        // and the same with MJBlocker
        mci::MJBlockerEstimator(Nmc, 1, x1D, &avg1D, &err1D);
        if (verbose) { reportAvgErr1D("MJBlockerEstimator()", avg1D, err1D); }
        assert(fabs(avg1D - refAvg[i]) < TINY); // this difference should be TINY
        assert(fabs(avg1D - refAvg[i]) < 3*err1D);
    }


    // --- check ND estimators ---
    if (verbose) { cout << endl << "Multidimensional versions of Estimators:" << endl << endl; }

    // result arrays to pass
    double avgND[nd], errND[nd];

    mci::MultiDimUncorrelatedEstimator(Nmc, nd, xND, avgND, errND);
    if (verbose) { reportAvgErrND("MultiDimUncorrelatedEstimator()", nd, avgND, errND); }
    assertArraysEqual(nd, avgND, refAvg, EXTRA_TINY); // these should be virtually identical

    mci::MultiDimBlockEstimator(Nmc, nd, xND, nblocks, avgND, errND);
    if (verbose) { reportAvgErrND("MultiDimBlockEstimator()", nd, avgND, errND); }
    assertArraysEqual(nd, avgND, refAvg, EXTRA_TINY); // these should be virtually identical

    mci::MultiDimFCBlockerEstimator(Nmc, nd, xND, avgND, errND);
    if (verbose) { reportAvgErrND("MultiDimFCBlockerEstimator()", nd, avgND, errND); }
    for (int i = 0; i < nd; ++i) {
        assert(fabs(avgND[i] - refAvg[i]) < TINY); // this difference should be TINY
        assert(fabs(avgND[i] - refAvg[i]) < 3*errND[i]); // like in the 1D case
    }

    mci::MJBlockerEstimator(Nmc, nd, xND, avgND, errND);
    if (verbose) { reportAvgErrND("MJBlockerEstimator()", nd, avgND, errND); }
    for (int i = 0; i < nd; ++i) {
        assert(fabs(avgND[i] - refAvg[i]) < TINY); // this difference should be TINY
        assert(fabs(avgND[i] - refAvg[i]) < 3*errND[i]); // like in the 1D case
    }


    // --- check accumulators ---
    if (verbose) { cout << endl << "Now using accumulator classes to store data:" << endl << endl; }
    XND obsfun(nd); // n-dimensional position observable
    SimpleAccumulator simpleAccu(obsfun, 1);
    SimpleAccumulator simpleAccuSkip2(obsfun, 2);
    BlockAccumulator blockAccu(obsfun, 1, 16);
    BlockAccumulator blockAccuSkip2(obsfun, 2, 8);
    FullAccumulator fullAccu(obsfun, 1);
    FullAccumulator fullAccuSkip2(obsfun, 2);

    vector<pair<AccumulatorInterface *, string> > accuList;
    accuList.emplace_back(&simpleAccu, "simpleAccu");
    accuList.emplace_back(&blockAccu, "blockAccu");
    accuList.emplace_back(&fullAccu, "fullAccu");
    accuList.emplace_back(&simpleAccuSkip2, "simpleAccuSkip2");
    accuList.emplace_back(&blockAccuSkip2, "blockAccuSkip2");
    accuList.emplace_back(&fullAccuSkip2, "fullAccuSkip2");

    for (auto &accuTup : accuList) {
        if (verbose) { cout << endl << "Checking accumulator " << accuTup.second << " ..." << endl; }
        checkAccumulator(*accuTup.first, Nmc, nd, xND, accepted, nchanged, changedIdx, SMALL, verbose);
    }

    // check that values with same skip level are very equal
    assertAccuAveragesEqual(simpleAccu, blockAccu, EXTRA_TINY);
    assertAccuAveragesEqual(simpleAccu, fullAccu, EXTRA_TINY);

    assertAccuAveragesEqual(simpleAccuSkip2, blockAccuSkip2, EXTRA_TINY);
    assertAccuAveragesEqual(simpleAccuSkip2, fullAccuSkip2, EXTRA_TINY);
}
