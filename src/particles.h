#ifndef PARTICLES_H
#define PARTICLES_H

#include "pfuclt_aux.h"

#include <vector>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <boost/random.hpp>
#include <boost/thread/mutex.hpp>
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <tf2_ros/transform_broadcaster.h>

#include <read_omni_dataset/RobotState.h>
#include <read_omni_dataset/LRMGTData.h>
#include <pfuclt_omni_dataset/particle.h>
#include <pfuclt_omni_dataset/particles.h>

#include <dynamic_reconfigure/server.h>
#include <pfuclt_omni_dataset/DynamicConfig.h>

// ideally later this will be a parameter, when it makes sense to
#define STATES_PER_TARGET 3

// offsets
#define O_X (0)
#define O_Y (1)
#define O_THETA (2)
//#define O_TARGET (nRobots_ * nStatesPerRobot_)
#define O_TX (0)
#define O_TY (1)
#define O_TZ (2)
//#define O_WEIGHT (nSubParticleSets_ - 1)

// target motion model and estimator
#define MAX_ESTIMATOR_STACK_SIZE 15
#define TARGET_RAND_MEAN 0
#define TARGET_RAND_STDDEV 20.0

// concerning time
#define TARGET_ITERATION_TIME_DEFAULT 0.0333
#define TARGET_ITERATION_TIME_MAX (1)

// others
#define MIN_WEIGHTSUM 1e-10
#define RESAMPLE_START_AT 0.5

//#define MORE_DEBUG true

namespace pfuclt_ptcls
{

using namespace pfuclt_aux;

typedef float pdata_t;

typedef double (*estimatorFunc)(const std::vector<double>&,
                                const std::vector<double>&);

typedef struct odometry_s
{
  double x, y, theta;
} Odometry;

typedef struct landmarkObs_s
{
  bool found;
  double x, y;
  double d, phi;
  double covDD, covPP, covXX, covYY;
  landmarkObs_s() { found = false; }
} LandmarkObservation;

typedef struct targetObs_s
{
  bool found;
  double x, y, z;
  double d, phi;
  double covDD, covPP, covXX, covYY;

  targetObs_s() { found = false; }
} TargetObservation;

// Apply concept of subparticles (the particle set for each dimension)
typedef std::vector<pdata_t> subparticles_t;
typedef std::vector<subparticles_t> particles_t;

// This will be the generator use for randomizing
typedef boost::random::mt19937 RNGType;

class ParticleFilter
{
private:
  boost::mutex mutex_;
  dynamic_reconfigure::Server<pfuclt_omni_dataset::DynamicConfig>
      dynamicServer_;

protected:
  /**
   * @brief dynamicReconfigureCallback - Dynamic reconfigure callback for
   * dynamically setting variables during runtime
   * @remark class members as classbacks must be defined as static so they don't
   * have access to the "this" keyword, they must be member-free methods. Thus,
   * the callback is called with a pointer to a ParticleFilter object (this),
   * and has private access since it is a method of the same class
   */
  static void dynamicReconfigureCallback(pfuclt_omni_dataset::DynamicConfig&,
                                         ParticleFilter*);

  /**
   * @brief The state_s struct - defines a structure to hold state information
   * for the particle filter class
   */
  struct State
  {
    uint nRobots;
    uint nStatesPerRobot;

    /**
     * @brief The robotState_s struct - saves information on the belief of a
     * robot's state
     */
    typedef struct robotState_s
    {
      std::vector<pdata_t> pose;
      pdata_t conf;

      robotState_s(uint poseSize) : pose(poseSize, 0.0), conf(0.0) {}
    } RobotState;
    std::vector<RobotState> robots;

    /**
     * @brief The targetState_s struct - saves information on the belief of the
     * target's state
     */
    struct targetState_s
    {
      std::vector<pdata_t> pos;
      std::vector<pdata_t> vel;

      targetState_s() : pos(STATES_PER_TARGET, 0.0), vel(STATES_PER_TARGET, 0.0)
      {
      }
    } target;

    /**
     * @brief The targetVelocityEstimator_s struct - will estimate the velocity
     * of a target using a custom function. The struct keeps vectors with the
     * latest avilable information, up until a max data amount is reached
     */
    struct targetVelocityEstimator_s
    {
      std::vector<double> timeVec;
      std::vector<std::vector<double> > posVec;
      estimatorFunc estimateVelocity;

      uint maxDataSize;
      uint timeInit;
      uint numberVels;

      targetVelocityEstimator_s(const uint numberVels, const uint maxDataSize,
                                estimatorFunc ptrFunc)
          : numberVels(numberVels), posVec(numberVels, std::vector<double>()),
            maxDataSize(maxDataSize)
      {
        estimateVelocity = ptrFunc;
      }

      void insert(const double timeData,
                  const std::vector<TargetObservation>& obsData,
                  const std::vector<RobotState>& robotStates)
      {
        pdata_t ballGlobal[3];
        bool readyToInsert = false;
        size_t size = robotStates.size();
        uint chosenRobot = 0;
        pdata_t maxConf = 0.0;

        // Choose the robot based on having found the ball and the maximum
        // confidence
        for (uint r = 0; r < size; ++r)
        {
          if (obsData[r].found)
          {
            // TODO these hard coded values.. change or what?
            if (robotStates[r].conf > maxConf &&
                (obsData[r].x < 4.0 && obsData[r].y < 4.0))
            {
              readyToInsert = true;
              chosenRobot = r;
              maxConf = robotStates[r].conf;
            }
          }
        }

        // If ball hasn't be seen, don't insert and just return
        if (!readyToInsert)
          return;

        // Pick the state and data from the chosen robot
        const RobotState& rs = robotStates[chosenRobot];
        const TargetObservation& obs = obsData[chosenRobot];
        // Calc. coordinates in global frame based on observation data and robot
        // state belief
        ballGlobal[O_TX] = rs.pose[O_X] + obs.x * cos(rs.pose[O_THETA]) -
                           obs.y * sin(rs.pose[O_THETA]);
        ballGlobal[O_TY] = rs.pose[O_Y] + obs.x * sin(rs.pose[O_THETA]) +
                           obs.y * cos(rs.pose[O_THETA]);
        ballGlobal[O_TZ] = obs.z;

        if (timeVec.empty())
          timeInit = ros::Time::now().toNSec() * 1e-9;

        timeVec.push_back(timeData - timeInit);

        for (uint velType = 0; velType < posVec.size(); ++velType)
          posVec[velType].push_back(ballGlobal[velType]);

        if (timeVec.size() > maxDataSize)
        {
          timeVec.erase(timeVec.begin());
          for (uint velType = 0; velType < posVec.size(); ++velType)
            posVec[velType].erase(posVec[velType].begin());
        }
      }

      bool isReadyToEstimate() { return (timeVec.size() == maxDataSize); }

      double estimate(uint velType)
      {
        double velEst = estimateVelocity(timeVec, posVec[velType]);
        ROS_DEBUG("Estimated velocity type %d = %f", velType, velEst);

#ifdef MORE_DEBUG
        std::ostringstream oss_time;
        oss_time << "timeVec = [ ";
        for (uint i = 0; i < timeVec.size(); ++i)
          oss_time << timeVec[i] << " ";
        oss_time << "]";

        std::ostringstream oss_pos;
        oss_pos << "posVec[" << velType << "] = [ ";
        for (uint i = 0; i < posVec[velType].size(); ++i)
          oss_pos << posVec[velType][i] << " ";
        oss_pos << "]";

        ROS_DEBUG("%s", oss_time.str().c_str());
        ROS_DEBUG("%s", oss_pos.str().c_str());
#endif
        return velEst;
      }
    } targetVelocityEstimator;

    /**
     * @brief State - constructor
     */
    State(const uint nStatesPerRobot, const uint nRobots)
        : nStatesPerRobot(nStatesPerRobot), nRobots(nRobots),
          targetVelocityEstimator(STATES_PER_TARGET, MAX_ESTIMATOR_STACK_SIZE,
                                  pfuclt_aux::linearRegressionSlope)
    {
      // Create and initialize the robots vector
      for (uint r = 0; r < nRobots; ++r)
        robots.push_back(robotState_s(nStatesPerRobot));
    }

  private:
    void print(std::ostringstream& oss, std::vector<bool>& vec)
    {
      oss << "[";

      for (std::vector<bool>::iterator it = vec.begin(); it != vec.end(); ++it)
        oss << *it;

      oss << "]";
      return;
    }

  public:
    void print()
    {
      std::ostringstream oss;
      oss << "PF State:" << std::endl;
      for (uint r = 0; r < nRobots; ++r)
      {
        oss << "OMNI " << r + 1 << "[ ";
        for (uint k = 0; k < nStatesPerRobot; ++k)
          oss << robots[r].pose[k] << " ";
        oss << "]" << std::endl;
      }

      oss << "Target [ ";
      for (uint k = 0; k < STATES_PER_TARGET; ++k)
        oss << target.pos[k] << " ";
      oss << "]" << std::endl;

      ROS_DEBUG("%s", oss.str().c_str());
    }
  };

public:
  /**
   * @brief The PFinitData struct - provides encapsulation to the initial data
   * necessary to construct a ParticleFilter instance
   */
  struct PFinitData
  {
    const uint mainRobotID, nParticles, nTargets, statesPerRobot, nRobots,
        nLandmarks;
    const std::vector<bool>& robotsUsed;
    const std::vector<Landmark>& landmarksMap;
    std::vector<float> alpha;

    /**
     * @brief PFinitData
     * @param mainRobotID - the robot number where this algorithm will run on -
     * affects the timings of iteration and estimation updates - consider that
     * OMNI1 is ID1
     * @param nParticles - the number of particles to be in the particle filter
     * @param nTargets - the number of targets to consider
     * @param statesPerRobot - the state space dimension for each robot
     * @param nRobots - number of robots
     * @param nLandmarks - number of landmarks
     * @param robotsUsed - vector of bools mentioning if robots are being used,
     * according to the standard robot ordering
     * @param landmarksMap - vector of Landmark structs containing information
     * on the landmark locations
     * @param vector with values to be used in the RNG for the model sampling
     */
    PFinitData(const uint mainRobotID, const uint nParticles,
               const uint nTargets, const uint statesPerRobot,
               const uint nRobots, const uint nLandmarks,
               const std::vector<bool>& robotsUsed,
               const std::vector<Landmark>& landmarksMap,
               const std::vector<float>& alpha = std::vector<float>())
        : mainRobotID(mainRobotID), nParticles(nParticles), nTargets(nTargets),
          statesPerRobot(statesPerRobot), nRobots(nRobots),
          nLandmarks(nLandmarks), alpha(alpha), robotsUsed(robotsUsed),
          landmarksMap(landmarksMap)
    {
      // If vector alpha is not provided, use a default one
      if (this->alpha.empty())
      {
        for (int r = 0; r < nRobots; ++r)
        {
          this->alpha.push_back(0.015);
          this->alpha.push_back(0.1);
          this->alpha.push_back(0.5);
          this->alpha.push_back(0.001);
        }
      }

      // Check size of vector alpha
      if (this->alpha.size() != 4 * nRobots)
      {
        ROS_ERROR(
            "The provided vector alpha is not of the correct size. Returning "
            "without particle filter! (should have %d=nRobots*4 elements)",
            nRobots * 4);
        return;
      }
    }
  };

protected:
  const uint mainRobotID_;
  const std::vector<Landmark>& landmarksMap_;
  const std::vector<bool>& robotsUsed_;
  const uint nParticles_;
  const uint nTargets_;
  const uint nRobots_;
  const uint nStatesPerRobot_;
  const uint nSubParticleSets_;
  const uint nLandmarks_;
  particles_t particles_;
  particles_t weightComponents_;
  RNGType seed_;
  std::vector<float> alpha_;
  bool initialized_;
  std::vector<std::vector<LandmarkObservation> > bufLandmarkObservations_;
  std::vector<TargetObservation> bufTargetObservations_;
  TimeEval targetIterationTime_, odometryTime_, iterationTime_;
  struct State state_;

  /**
   * @brief copyParticle - copies a whole particle from one particle set to
   * another
   * @param p_To - the destination particle set
   * @param p_From - the origin particle set
   * @param i_To - the index of the particle to copy to
   * @param i_From - the index of the particle to copy from
   * @remark Make sure the sizes of p_To and p_From are the same
   */
  inline void copyParticle(particles_t& p_To, particles_t& p_From, uint i_To,
                           uint i_From)
  {
    copyParticle(p_To, p_From, i_To, i_From, 0, p_To.size() - 1);
  }

  /**
   * @brief copyParticle - copies some subparticle sets of a particle from one
   * particle set to another
   * @param p_To - the destination particle set
   * @param p_From - the origin particle set
   * @param i_To - the index of the particle to copy to
   * @param i_From - the index of the particle to copy from
   * @param subFirst - the first subparticle set index
   * @param subLast - the last subparticle set index
   * @remark Make sure the sizes of p_To and p_From are the same
   */
  inline void copyParticle(particles_t& p_To, particles_t& p_From, uint i_To,
                           uint i_From, uint subFirst, uint subLast)
  {
    for (uint k = subFirst; k <= subLast; ++k)
      p_To[k][i_To] = p_From[k][i_From];
  }

  /**
   * @brief resetWeights - assign the value val to all particle weights
   */
  inline void resetWeights(pdata_t val)
  {
    particles_[O_WEIGHT].assign(particles_[O_WEIGHT].size(), val);
  }

  /**
   * @brief predictTarget - predict target state step
   * @param robotNumber - the robot performing, for debugging purposes
   */
  void predictTarget();

  /**
   * @brief fuseRobots - fuse robot states step
   */
  void fuseRobots();

  /**
   * @brief fuseTarget - fuse target state step
   */
  void fuseTarget();

  /**
   * @brief modifiedMultinomialResampler - a costly resampler that keeps 50% of
   * the particles and implements the multinomial resampler on the rest
   */
  void modifiedMultinomialResampler(uint startAt);

  /**
   * @brief resample - the resampling step
   */
  void resample();

  /**
   * @brief resample - state estimation through weighted means, and linear
   * regression for the target velocity
   */
  void estimate();

  /**
   * @brief nextIteration - perform final steps before next iteration
   */
  virtual void nextIteration() {}

public:
  boost::shared_ptr<std::ostringstream> iteration_oss;
  uint O_TARGET, O_WEIGHT;

  /**
   * @brief ParticleFilter - constructor
   * @param data - reference to a struct of PFinitData containing the necessary
   * information to construct the Particle Filter
   */
  ParticleFilter(struct PFinitData& data);

  /**
   * @brief updateTargetIterationTime - the main robot should call this method
   * after the target callback
   * @param tRos - time variable to be used in calculating the target's
   * iteration time
   */
  void updateTargetIterationTime(ros::Time tRos)
  {
    targetIterationTime_.updateTime(tRos);

    if (fabs(targetIterationTime_.diff) > TARGET_ITERATION_TIME_MAX)
    {
      // Something is wrong, set to default iteration time
      targetIterationTime_.diff = TARGET_ITERATION_TIME_DEFAULT;
    }
    ROS_DEBUG("Target tracking iteration time: %f", targetIterationTime_.diff);
  }

  /**
   * @brief getPFReference - retrieve a reference to this object - to be
   * overloaded by deriving classes so that the base class can be returned
   * @return reference to this object
   */
  ParticleFilter* getPFReference() { return this; }

  /**
   * @brief printWeights
   */
  void printWeights(std::string pre);

  /**
   * @brief assign - assign a value to every particle in all subsets
   * @param value - the value to assign
   */
  void assign(const pdata_t value);

  /**
   * @brief assign - assign a value to every particle in one subset
   * @param value - the value to assign
   * @param index - the subset index [0,N]
   */
  void assign(const pdata_t value, const uint index);

  /**
   * @brief operator [] - array subscripting access to the private particle set
   * @param index - the subparticle set index number to access
   * @return the subparticles_t object reference located at particles_[index]
   */
  inline subparticles_t& operator[](int index) { return particles_[index]; }

  /**
   * @brief operator [] - const version of the array subscripting access, when
   * using it on const intantiations of the class
   * @param index - the subparticle set index number to access
   * @return a const subparticles_t object reference located at
   * particles_[index]
   */
  inline const subparticles_t& operator[](int index) const
  {
    return particles_[index];
  }

  /**
   * @brief init - initialize the particle filter set with the default
   * randomized values
   */
  void init();

  /**
   * @brief init - initialize the particle filter set with custom values
   * @param customRandInit - vector of doubles with the following form: <lvalue,
   * rvalue,
   * lvalue, rvalue, ...>
   * which will be used as the limits of the uniform distribution.
   * This vector should have a size equal to twice the number of dimensions
   * @param customPosInit - vector of doubles with the following form
   * <x,y,theta,x,y,theta,...> with size equal to nStatesPerRobot*nRobots
   */
  void init(const std::vector<double>& customRandInit,
            const std::vector<double>& customPosInit);

  /**
   * @brief predict - prediction step in the particle filter set with the
   * received odometry
   * @param robotNumber - the robot number [0,N]
   * @param odometry - a structure containing the latest odometry readings
   * @param time - a ros::Time structure with the timestamp for this data
   * @warning only for the omni dataset configuration
   */
  void predict(const uint robotNumber, const Odometry odom,
               const ros::Time stamp);

  /**
   * @brief isInitialized - simple interface to access private member
   * initialized_
   * @return true if particle filter has been initialized, false otherwise
   */
  bool isInitialized() { return initialized_; }

  /**
   * @brief size - interface to the size of the particle filter
   * @return - the number of subparticle sets
   */
  std::size_t size() { return nSubParticleSets_; }

  /**
   * @brief saveLandmarkObservation - saves the landmark observation to a buffer
   * of
   * observations
   * @param robotNumber - the robot number in the team
   * @param landmarkNumber - the landmark serial id
   * @param obs - the observation data as a structure defined in this file
   */
  inline void saveLandmarkObservation(const uint robotNumber,
                                      const uint landmarkNumber,
                                      const LandmarkObservation obs)
  {
    bufLandmarkObservations_[robotNumber][landmarkNumber] = obs;
  }

  /**
   * @brief saveLandmarkObservation - change the measurement buffer state
   * @param robotNumber - the robot number in the team
   * @param found - whether this landmark has been found
   */
  inline void saveLandmarkObservation(const uint robotNumber,
                                      const uint landmarkNumber,
                                      const bool found)
  {
    bufLandmarkObservations_[robotNumber][landmarkNumber].found = found;
  }

  /**
   * @brief saveAllLandmarkMeasurementsDone - call this function when all
   * landmark measurements have
   * been performed by a certain robot
   * @param robotNumber - the robot number performing the measurements
   */
  void saveAllLandmarkMeasurementsDone(const uint robotNumber);

  /**
   * @brief saveTargetObservation - saves the target observation to a buffer of
   * observations
   * @param robotNumber - the robot number in the team
   * @param obs - the observation data as a structure defined in this file
   */
  inline void saveTargetObservation(const uint robotNumber,
                                    const TargetObservation obs)
  {
    bufTargetObservations_[robotNumber] = obs;
  }

  /**
   * @brief saveTargetObservation - change the measurement buffer state
   * @param robotNumber - the robot number in the team
   * @param found - whether the target has been found
   */
  inline void saveTargetObservation(const uint robotNumber, const bool found)
  {
    bufTargetObservations_[robotNumber].found = found;
  }

  /**
   * @brief saveAllTargetMeasurementsDone - call this function when all target
   * measurements have
   * been performed by a certain robot
   * @param robotNumber - the robot number performing the measurements
   */
  void saveAllTargetMeasurementsDone(const uint robotNumber);
};

/**
 * @brief The PFPublisher class - implements publishing for the ParticleFilter
 * class using ROS
 */
class PFPublisher : public ParticleFilter
{
public:
  struct PublishData
  {
    ros::NodeHandle& nh;
    float robotHeight;

    /**
     * @brief PublishData - contains information necessary for the PFPublisher
     * class
     * @param nh - the node handle object
     * @param robotHeight - the fixed robot height
     */
    PublishData(ros::NodeHandle& nh, float robotHeight)
        : nh(nh), robotHeight(robotHeight)
    {
    }
  };

private:
  ros::Subscriber GT_sub_;
  ros::Publisher robotStatePublisher_, targetStatePublisher_,
      particlePublisher_, syncedGTPublisher_, targetEstimatePublisher_,
      targetGTPublisher_, targetParticlePublisher_;
  std::vector<ros::Publisher> particleStdPublishers_;
  std::vector<ros::Publisher> robotGTPublishers_;
  std::vector<ros::Publisher> robotEstimatePublishers_;

  read_omni_dataset::LRMGTData msg_GT_;
  pfuclt_omni_dataset::particles msg_particles_;
  read_omni_dataset::RobotState msg_state_;
  read_omni_dataset::BallData msg_target_;

  std::vector<tf2_ros::TransformBroadcaster> robotBroadcasters;

  struct PublishData pubData;

  void publishParticles();
  void publishRobotStates();
  void publishTargetState();
  void publishGTData();

public:
  /**
   * @brief PFPublisher - constructor
   * @param data - a structure with the necessary initializing data for the
   * ParticleFilter class
   * @param publishData - a structure with some more data for this class
   */
  PFPublisher(struct ParticleFilter::PFinitData& data,
              struct PublishData publishData);

  /**
   * @brief getPFReference - retrieve a reference to the base class's members
   * @remark C++ surely is awesome
   * @return returns a reference to the base ParticleFilter for this object
   */
  ParticleFilter* getPFReference() { return (ParticleFilter*)this; }

  /**
   * @brief gtDataCallback - callback of ground truth data
   */
  void gtDataCallback(const read_omni_dataset::LRMGTData::ConstPtr&);

  /**
   * @brief nextIteration - extends the base class method to add the ROS
   * publishing
   */
  void nextIteration();
};

// end of namespace pfuclt_ptcls
}
#endif // PARTICLES_H
