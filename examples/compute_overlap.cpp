// kate: replace-tabs off; indent-width 4; indent-mode normal
// vim: ts=4:sw=4:noexpandtab
/*

Copyright (c) 2010--2012,
François Pomerleau and Stephane Magnenat, ASL, ETHZ, Switzerland
You can contact the authors at <f dot pomerleau at gmail dot com> and
<stephane at magnenat dot net>

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL ETH-ASL BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "pointmatcher/PointMatcher.h"
#include "pointmatcher/IO.h"
#include <cassert>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <boost/format.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/lexical_cast.hpp>

using namespace std;
using namespace PointMatcherSupport;

void validateArgs(int argc, char *argv[]);
void setupArgs(int argc, char *argv[], unsigned int& startId, unsigned int& endId, string& extension);

/**
  * Code example for DataFilter taking a sequence of point clouds with  
  * their global coordinates and build a map with a fix (manageable) number of points
  */
int main(int argc, char *argv[])
{
	validateArgs(argc, argv);

	typedef PointMatcher<float> PM;
	typedef PointMatcherIO<float> PMIO;
	typedef PM::Matrix Matrix;
	typedef PM::TransformationParameters TP;
	typedef PM::DataPoints DP;
	typedef PM::Matches Matches;

	// Process arguments
	PMIO::FileInfoVector list(argv[1]);
	bool debugMode = false;
	if (argc == 4)
		debugMode = true;
	
	if(debugMode)
		setLogger(PM::get().LoggerRegistrar.create("FileLogger"));

	// Prepare transformation chain for maps
	PM::Transformation* transformPoints;
	transformPoints = PM::get().TransformationRegistrar.create("TransformFeatures");
	
	PM::Transformation* transformNormals;
	transformNormals = PM::get().TransformationRegistrar.create("TransformNormals");
	
	PM::Transformations transformations;
	transformations.push_back(transformPoints);
	transformations.push_back(transformNormals);

	DP reading, reference;
	TP Tread = TP::Identity(4,4);
	DP mapCloud;
	TP Tref = TP::Identity(4,4);

	//TODO: loop through all point clouds
	//int i = 0; // reading
	//int j = 0; // reference

	unsigned startingI = 0;
	//unsigned listSizeI = list.size();
	//unsigned listSizeJ = list.size();
	unsigned listSizeI = 3;
	unsigned listSizeJ = 3;
	if(debugMode)
	{
		startingI = boost::lexical_cast<unsigned>(argv[2]);
		listSizeI = startingI + 1;
	}

	PM::Matrix overlapResults = PM::Matrix::Ones(listSizeJ, listSizeI);

	for(unsigned i = startingI; i < listSizeI; i++)
	{
		unsigned startingJ = i+1;
		if(debugMode)
		{
			startingJ = boost::lexical_cast<unsigned>(argv[3]);
			listSizeJ = startingJ + 1;
		}
		for(unsigned j = startingJ; j < listSizeJ; j++)
		{
			// Load point clouds
			reading = DP::load(list[i].readingFileName);
			reference = DP::load(list[j].readingFileName);

			cout << "Point cloud loaded" << endl;

			// Load transformation matrices
			if(list[i].groundTruthTransformation.rows() != 0)
			{
				Tread = list[i].groundTruthTransformation;
				Tref = list[j].groundTruthTransformation;
			}
			else
			{
				cout << "ERROR: fields gTXX (ground truth) is required" << endl;
				abort();
			}

			// Move point cloud in global frame
			transformations.apply(reading, Tread);
			transformations.apply(reference, Tref);

			// Preprare filters
			PM::DataPointsFilter* subSample(
				PM::get().DataPointsFilterRegistrar.create(
					"RandomSamplingDataPointsFilter", 
					map_list_of
						("prob", "0.5")
				)
			);

			PM::DataPointsFilter* maxDensity(
				PM::get().DataPointsFilterRegistrar.create(
					"MaxDensityDataPointsFilter"
				)
			);
			
			/*PM::DataPointsFilter* cutInHalf;
			cutInHalf = PM::get().DataPointsFilterRegistrar.create(
				"MinDistDataPointsFilter", PM::Parameters({
					{"dim", "1"},
					{"minDist", "0"}
				}));*/

			PM::DataPointsFilter* computeDensity(
				PM::get().DataPointsFilterRegistrar.create(
					"SurfaceNormalDataPointsFilter", 
					map_list_of
						("knn", "20")
						("keepDensities", "1")
				)
			);

			reading = subSample->filter(reading);
			reading = computeDensity->filter(reading);
			reading = maxDensity->filter(reading);
			//reading = cutInHalf->filter(reading);
			const Matrix inliersRead = Matrix::Zero(1, reading.features.cols());
			reading.addDescriptor("inliers", inliersRead);

			reference = subSample->filter(reference);
			reference = computeDensity->filter(reference);
			reference = maxDensity->filter(reference);
			const Matrix inliersRef = Matrix::Zero(1, reference.features.cols());
			reference.addDescriptor("inliers", inliersRef);

			//TODO: reverse self and target
			DP self = reading;
			DP target = reference;

			for(int l = 0; l < 2; l++)
			{
				const int selfPtsCount = self.features.cols();
				const int targetPtsCount = target.features.cols();

				// Build kd-tree
				int knn = 20;
				int knnAll = 50;
				PM::Matcher* matcherSelf(
					PM::get().MatcherRegistrar.create(
						"KDTreeMatcher",
						map_list_of
							("knn", toParam(knn))
					)
				);

				PM::Matcher* matcherTarget(
					PM::get().MatcherRegistrar.create(
						"KDTreeVarDistMatcher",
						map_list_of
							("knn", toParam(knnAll))
							("maxDistField", "maxSearchDist")
					)
				);

				matcherSelf->init(self);
				matcherTarget->init(target);

				Matches selfMatches(knn, selfPtsCount);
				selfMatches = matcherSelf->findClosests(self);

				const Matrix maxSearchDist = selfMatches.dists.colwise().maxCoeff().cwiseSqrt();
				self.addDescriptor("maxSearchDist", maxSearchDist);

				Matches targetMatches(knnAll, targetPtsCount);
				targetMatches = matcherTarget->findClosests(self);

				BOOST_AUTO(inlierSelf, self.getDescriptorViewByName("inliers"));
				BOOST_AUTO(inlierTarget, target.getDescriptorViewByName("inliers"));
				for(int i = 0; i < selfPtsCount; i++)
				{
					for(int k = 0; k < knnAll; k++)
					{
						if (targetMatches.dists(k, i) != numeric_limits<float>::infinity())
						{
							inlierSelf(0,i) = 1.0;
							inlierTarget(0,targetMatches.ids(k, i)) = 1.0;
						}
					}
				}
				
				// Swap point clouds
				PM::swapDataPoints(self, target);
			}
			
			const BOOST_AUTO(finalInlierSelf, self.getDescriptorViewByName("inliers"));
			const BOOST_AUTO(finalInlierTarget, target.getDescriptorViewByName("inliers"));
			const double selfRatio = (finalInlierSelf.array() > 0).count()/(double)finalInlierSelf.cols();
			const double targetRatio = (finalInlierTarget.array() > 0).count()/(double)finalInlierTarget.cols();
			
			cout << i << " -> " << j << ": " << selfRatio << endl;
			cout << j << " -> " << i << ": " << targetRatio << endl;
			
			if(debugMode)
			{
				self.save("scan_i.vtk");
				target.save("scan_j.vtk");
			}
			else
			{
				overlapResults(j,i) = selfRatio;
				overlapResults(i,j) = targetRatio;
			}
		}
	}


	// write results in a file
	std::fstream outFile;
	outFile.open("overlapResults.csv", fstream::out);
	for(int x=0; x < overlapResults.rows(); x++)
	{
		for(int y=0; y < overlapResults.cols(); y++)
		{
			outFile << overlapResults(x, y) << ", ";
		}

		outFile << endl;
	}

	outFile.close();

	return 0;
}

void validateArgs(int argc, char *argv[])
{
	if (!(argc == 2 || argc == 4))
	{
		cerr << "\nError in command line, usage " << argv[0] << " listOfFiles.csv <i j>" << endl;
		cerr << "\ni and j are optional arguments. If used, only compute the overlap for those 2 point cloud ids and dump VTK files for visual inspection." << endl;
		abort();
	}
}



