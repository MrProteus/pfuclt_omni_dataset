#include "pfuclt_aux.h"
#include "particles.h"
#include "pfuclt_omni_dataset.h"

namespace pfuclt
{

RobotFactory::RobotFactory(ros::NodeHandle& nh)
  : nh_(nh), pfParticles(pfuclt_ptcls::particles(N_PARTICLES, N_DIMENSIONS))
{
  for (uint rn = 0; rn < MAX_ROBOTS; rn++)
  {
    if (PLAYING_ROBOTS[rn])
    {
      Eigen::Isometry2d initialRobotPose(
            Eigen::Rotation2Dd(-M_PI).toRotationMatrix());
      initialRobotPose.translation() =
          Eigen::Vector2d(POS_INIT[2 * rn + 0], POS_INIT[2 * rn + 1]);

      if (rn + 1 == MY_ID)
      {
        robots_.push_back(
              new SelfRobot(nh_, initialRobotPose, pfParticles, this, rn));
      }
      else
      {
        robots_.push_back(
              new TeammateRobot(nh_, initialRobotPose, pfParticles, this, rn));
      }
    }
  }
}

RobotFactory::~RobotFactory()
{
  // clean up - delete all variables in heap
  for (std::vector<Robot*>::iterator it = robots_.begin(); it != robots_.end();
       ++it)
    delete *it;

  robots_.empty();

  // shutdown function will call SIGINT on this node
  nh_.shutdown();
}

void RobotFactory::initializeFixedLandmarks()
{
  std::string filename;
  using namespace pfuclt_aux;

  // get the filename from parameter server
  if (!readParam<std::string>(nh_, "/LANDMARKS_CONFIG", filename))
    return;

  // parse the file and copy to vector of Landmarks
  landmarks = getLandmarks(filename.c_str());
  ROS_ERROR_COND(landmarks.empty(), "Couldn't open file \"%s\"",
                 filename.c_str());

  // iterate over the vector and print information
  for (std::vector<Landmark>::iterator it = landmarks.begin();
       it != landmarks.end(); ++it)
  {
    ROS_INFO("A fixed landmark with ID %d at position {x=%.2f, y=%.2f} \twas "
             "created",
             it->serial, it->x, it->y);
  }
}

bool RobotFactory::areAllRobotsActive()
{
  for (std::vector<Robot*>::iterator it = robots_.begin(); it != robots_.end();
       ++it)
  {
    if (!(*it)->isStarted())
      return false;
  }
  return true;
}

SelfRobot::SelfRobot(ros::NodeHandle& nh, Eigen::Isometry2d initPose,
                     particles& ptcls, RobotFactory* caller, uint robotNumber)
  : Robot(nh, caller, initPose, ptcls, robotNumber), seed_(time(0))
{
  // Prepare particle message
  msg_particles.particles.resize(N_PARTICLES);
  for(uint part=0; part<N_PARTICLES; ++part)
    msg_particles.particles[part].particle.resize(N_DIMENSIONS);

  // Subscribe to topics
  sOdom_ = nh.subscribe<nav_msgs::Odometry>(
        "/omni" + boost::lexical_cast<std::string>(robotNumber + 1) + "/odometry",
        10,
        boost::bind(&SelfRobot::selfOdometryCallback, this, _1, robotNumber + 1));

  sBall_ = nh.subscribe<read_omni_dataset::BallData>(
        "/omni" + boost::lexical_cast<std::string>(robotNumber + 1) +
        "/orangeball3Dposition",
        10, boost::bind(&SelfRobot::selfTargetDataCallback, this, _1,
                        robotNumber + 1));

  sLandmark_ = nh.subscribe<read_omni_dataset::LRMLandmarksData>(
        "/omni" + boost::lexical_cast<std::string>(robotNumber + 1) +
        "/landmarkspositions",
        10, boost::bind(&SelfRobot::selfLandmarkDataCallback, this, _1,
                        robotNumber + 1));

  // The Graph Generator ans solver should also subscribe to the GT data and
  // publish it... This is important for time synchronization
  GT_sub_ = nh.subscribe<read_omni_dataset::LRMGTData>(
        "gtData_4robotExp", 10,
        boost::bind(&SelfRobot::gtDataCallback, this, _1));
  // GT_sub_ = nh->subscribe("gtData_4robotExp", 1000, gtDataCallback);

  ROS_INFO(" constructing SelfRobot <<object>> and called sensor subscribers "
           "for this robot %d",
           robotNumber + 1);

  // Advertise some topics; don't change the names, preferably remap in a
  // launch file
  State_publisher =
      nh.advertise<read_omni_dataset::RobotState>("/pfuclt_omni_poses", 1000);

  targetStatePublisher = nh.advertise<read_omni_dataset::BallData>(
        "/pfuclt_orangeBallState", 1000);

  virtualGTPublisher = nh.advertise<read_omni_dataset::LRMGTData>(
        "/gtData_synced_pfuclt_estimate", 1000);

  particlePublisher =
      nh.advertise<pfuclt_omni_dataset::particles>("/pfuclt_particles", 10);

  // Mark initialized flags as false
  setStarted(false);
  particlesInitialized = false;

  // Resize particle set to allow all subparticles
  for (int i = 0; i < 19; i++)
  {
    particleSet_[i].resize(N_PARTICLES);
  }
}

void SelfRobot::initPFset()
{

  const float particleSetRandomInitValues[18][2] = {
    // OMNI 1
    // 0,1,2
    { 0, 6.0 },
    { -4.5, 4.5 },
    { -PI, PI },

    // OMNI 2
    // 3,4,5
    { 0, 6.0 },
    { -4.5, 4.5 },
    { -PI, PI },

    // OMNI 3
    // 6,7,8
    { 0, 6.0 },
    { -4.5, 4.5 },
    { -PI, PI },

    // OMNI 4
    // 9,10,11
    { POS_INIT[6] - 0.5, POS_INIT[6] + 0.5 },
    { POS_INIT[7] - 0.5, POS_INIT[7] + 0.5 },
    { PI - 0.001, PI + 0.001 },

    // OMNI 5
    // 12,13,14
    { 0, 6.0 },
    { -4.5, 4.5 },
    { -PI, PI },

    // Ball
    // 15,16,17
    { 0, 6.0 },
    { -4.5, 4.5 },
    { -PI, PI },
  };

  // Sample from a uniform distribution for every particle subset, according to
  // the values defined in particleSetRandomInitValues
  for (int i = 0; i < 18; ++i)
  {
    boost::random::uniform_real_distribution<> dist(
          particleSetRandomInitValues[i][0], particleSetRandomInitValues[i][1]);
    BOOST_FOREACH (float& f, particleSet_[i])
    {
      f = dist(seed_);
    }

    // std::for_each(particleSet_[i].begin(), particleSet_[i].end(),
    // boost::bind<void>(dist(seed_), _1));
  }

  // Particle weights init with same weight
  particleSet_[18].assign(particleSet_[18].size(), 1 / N_PARTICLES);

  // Put into message
  for (int i = 0; i < N_PARTICLES; i++)
  {
    for (int j = 0; j < 19; j++)
    {
      msg_particles.particles[i].particle[j] = pfParticles_[j][i];
    }
  }

  //
  //     float tempParticles[3][nParticles_];
  //
  //     // for the robots
  //     for(int n=0; n<MAX_ROBOTS; n++ )
  //     {
  //       ippsRandUniform_Direct_32f (&tempParticles[0][0], nParticles_, 0,
  //       6.0, &seed_);
  //       ippsRandUniform_Direct_32f (&tempParticles[1][0], nParticles_, -4.5,
  //       4.5, &seed_);
  //       ippsRandUniform_Direct_32f (&tempParticles[2][0], nParticles_, -PI,
  //       PI, &seed_);
  //
  //       for(int i=0; i<nParticles_; i++)
  //       {
  // 	for(int j=0; j<3; j++)
  // 	{
  // 	  pfParticlesSelf[i][n*3+j] = tempParticles[j][i];
  // 	  particleSet_[n*3+j][i] = tempParticles[j][i];
  // 	  pfucltPtcls.particles[i].particle[n*3+j] = pfParticlesSelf[i][n*3+j];
  // 	}
  //       }
  //
  //     }
  //     // for that one target
  //     ippsRandUniform_Direct_32f (&tempParticles[0][0], nParticles_, 0, 6.0,
  //     &seed_);
  //     ippsRandUniform_Direct_32f (&tempParticles[1][0], nParticles_, -4.5,
  //     4.5, &seed_);
  //     ippsRandUniform_Direct_32f (&tempParticles[2][0], nParticles_, 0, 0.5,
  //     &seed_);
  //
  //     for(int i=0; i<nParticles_; i++)
  //     {
  //       pfParticlesSelf[i][(MAX_ROBOTS+1)*3] = 1.0; // initialize all weights
  //       to 1
  //       particleSet_[(MAX_ROBOTS+1)*3][i] = 1.0;
  //
  //       for(int j=0; j<3; j++)
  //       {
  // 	pfParticlesSelf[i][MAX_ROBOTS*3+j] = tempParticles[j][i];
  // 	particleSet_[MAX_ROBOTS*3+j][i] = tempParticles[j][i];
  // 	pfucltPtcls.particles[i].particle[MAX_ROBOTS*3+j] =
  // pfParticlesSelf[i][MAX_ROBOTS*3+j];
  //       }
  //     }
  //
  //     particlePublisher.publish(pfucltPtcls);
}

void SelfRobot::PFpredict() {}

void SelfRobot::PFfuseRobotInfo() {}

void SelfRobot::PFfuseTargetInfo() {}

void SelfRobot::PFresample()
{
  double stdX, stdY, stdTheta;

  for (int robNo = 0; robNo < 6;
       robNo++) // for 4 robots : OMNI4, OMNI3, OMNI1 and OMNI5 in a row
  {

    stdX = pfuclt_aux::calc_stdDev<float>(&particleSet_[0 + robNo * 3]);
    stdY = pfuclt_aux::calc_stdDev<float>(&particleSet_[1 + robNo * 3]);
    stdTheta = pfuclt_aux::calc_stdDev<float>(&particleSet_[2 + robNo * 3]);

    //?????????????????????????????? what next?
    //????????????????????????????????????????????????????
  }

  // Duplicate all particles
  std::vector<float> particlesDuplicate[19];
  for (int i = 0; i < 19; i++)
  {
    particlesDuplicate[i] = particleSet_[i];
  }

  // Duplicate particle weights and get sorted indices
  std::vector<unsigned int> sortedIndex =
      pfuclt_aux::order_index<float>(particleSet_[18]);

  // Re-arrange particles according to particle weight sorted indices
  for (int i = 0; i < N_PARTICLES; i++) // for each particle
  {
    size_t index = sortedIndex[i]; // get its new index

    for (int j = 0; j < 19;
         j++) // and use the duplicated partiles to sort the set
      particleSet_[j][i] = particlesDuplicate[j][index];

    // ROS_DEBUG("Particle at X = %f, Y = %f, Theta = %f has Weight = %f",
    // particleSet_[])
  }

  // Normalize the weights using the sum of all weights
  float weightSum =
      std::accumulate(particleSet_[18].begin(), particleSet_[18].end(), 0);

  //Put into message
  msg_particles.weightSum = (uint16_t) weightSum;

  if (weightSum == 0.0)
    ROS_WARN("WeightSum of Particles = %f\n", weightSum);
  else
  {
    ROS_DEBUG("WeightSum of Particles = %f\n", weightSum);
  }

  normalizedWeights = particleSet_[18];
  std::transform(normalizedWeights.begin(), normalizedWeights.end(),
                 normalizedWeights.begin(),
                 std::bind1st(std::divides<float>(), weightSum));

  // Duplicate particles for resampling
  for (int i = 0; i < 19; i++)
  {
    particlesDuplicate[i] = particleSet_[i];
  }

  // Implementing a very basic resampler... a particle gets selected
  // proportional to its weight and 50% of the top particles are kept
  float cumulativeWeights[N_PARTICLES];
  cumulativeWeights[0] = normalizedWeights[0];
  for (int par = 1; par < N_PARTICLES; par++)
  {
    cumulativeWeights[par] =
        cumulativeWeights[par - 1] + normalizedWeights[par];
  }

  for (int par = 0; par < 100; par++)
  {
    for (int k = 0; k < 19; k++)
    {
      particleSet_[k][par] = particlesDuplicate[k][par];
      pfParticles_[k][par] = particleSet_[k][par];
      msg_particles.particles[par].particle[k] = particleSet_[k][par];
    }
  }

  for (size_t r = 9; r < 12; r++)
  {
    boost::random::uniform_real_distribution<> dist(particleSet_[r][0] - 1.5,
        particleSet_[r][0] + 1.5);
    BOOST_FOREACH (float& f, particleSet_[r])
    {
      f = dist(seed_);
    }
  }

  for (int par = 100; par < N_PARTICLES; par++)
  {

    //     float randNo;
    //     ippsRandUniform_Direct_32f (&randNo, 1, 0, 1, &seed_);
    //
    //     int m=100;
    //     while(randNo>cumulativeWeights[m])
    //     {
    //       m++;
    //     }

    // printf("sampled particle = %d, with randNo = %f and cumulativeWeights[%d]
    // = %f\n",m,randNo,m,cumulativeWeights[m]);
    for (int k = 0; k < 19; k++)
    {
      // particleSet_[k][par] = particlesDuplicate[k][m];
      pfParticles_[k][par] = particleSet_[k][par];
      msg_particles.particles[par].particle[k] = particleSet_[k][par];
    }
  }

  // Low variance resampling exactly as implemented in the book Probabilistic
  // robotics by thrun and burgard
  // duplicate the particles to add only the selected ones to the new state
  //   float c, u;
  //   Ipp32f r;
  //   c = normalizedWeights[0];
  //   ippsRandUniform_Direct_32f (&r, 1, 0, 1.0 / nParticles_, &seed_);
  // //   cout << "The random number r for LVSampler is = " << r << endl <<
  // endl;
  //   int m, i = 1;
  //   for (m = 0; m < nParticles_; m++)
  //   {
  //
  //     //printf("Particle weight at %d = %f\n",m,normalizedWeights[m]);
  //     u = r + (m) * (1.0 / nParticles_);
  //     while (u > c)
  //     {
  //       i = i + 1;
  //       c = c + normalizedWeights[i];
  //     }
  //     if (i < nParticles_)
  //     {
  //       cout << "The sampled Particle is = " << i <<endl;
  //       for(int k=0; k<19;k++)
  // 	{
  // 	  particleSet_[k][m] = particlesDuplicate[k][i];
  // 	}
  //     }
  //   }
  // //   //cout<<"Max i is "<<i<<endl;

  /// TODO change this to a much more stable way of finding the mean of the
  /// cluster which will represent the ball position. Because it's not the
  /// highest weight particle which represent's the ball position
  //   for(int robNo = 0; robNo<5; robNo++)     // for 4 robots : OMNI4, OMNI3,
  //   OMNI1 and OMNI5 in a row
  //     {
  //       //Find the Weighted mean of the particles
  //
  //       Ipp32f weightedMeanXYZ[3];
  //       weightedMeanXYZ[0] = 0.0;
  //       weightedMeanXYZ[1] = 0.0;
  //       weightedMeanXYZ[2] = 0.0;
  //       Ipp32f SumOfWeights = 0.0;
  //       Ipp32f MaxWeight = 0.0;
  //       Ipp32f MinWeight = 1000.0;
  //
  //       int p;
  //       for (p= 0; p < nParticles_; p++)
  //       {
  // 	weightedMeanXYZ[0] = weightedMeanXYZ[0] + particleSet_[0+robNo*3][p] *
  // particleSet_[15][p];
  // 	weightedMeanXYZ[1] = weightedMeanXYZ[1] + particleSet_[1+robNo*3][p] *
  // particleSet_[15][p];
  // 	weightedMeanXYZ[2] = weightedMeanXYZ[2] + particleSet_[2+robNo*3][p] *
  // particleSet_[15][p];
  // 	SumOfWeights = SumOfWeights + particleSet_[15][p];
  // 	if(particleSet_[15][p]>MaxWeight)
  // 	  MaxWeight = particleSet_[15][p];
  // 	if(particleSet_[15][p]<MinWeight)
  // 	  MinWeight = particleSet_[15][p];
  //       }
  //       weightedMeanXYZ[0] = weightedMeanXYZ[0] / SumOfWeights;
  //       weightedMeanXYZ[1] = weightedMeanXYZ[1] / SumOfWeights;
  //       weightedMeanXYZ[2] = weightedMeanXYZ[2] / SumOfWeights;
  //
  //       CULTstateOMNI4[CULT_IterationCount_OMNI4].state[robNo][0] =
  //       weightedMeanXYZ[0];
  //       CULTstateOMNI4[CULT_IterationCount_OMNI4].state[robNo][1] =
  //       weightedMeanXYZ[1];
  //       CULTstateOMNI4[CULT_IterationCount_OMNI4].state[robNo][2] =
  //       weightedMeanXYZ[2];
  //
  //       Ipp32f meanWeight = SumOfWeights/nParticles_;
  //
  //       //particles scaled between 0 and 255 for coloring purpose
  //       for (p= 0; p < nParticles_; p++)
  //       {
  // 	particleSet_[15][p] =
  // 255*(particleSet_[15][p]-MinWeight)/(MaxWeight-MinWeight);
  //       }
  //
  //
  //     }
}

void SelfRobot::selfOdometryCallback(
    const nav_msgs::Odometry::ConstPtr& odometry, uint RobotNumber)
{
  started_ = true;

  uint seq = odometry->header.seq;
  prevTime_ = curTime_;
  curTime_ = odometry->header.stamp;

  ROS_DEBUG("Got odometry from self-robot (ID=%d) at time %d\n",
            RobotNumber, odometry->header.stamp.sec);

  // Below is an example how to extract the odometry from the message and use it
  // to propagate the robot state by simply concatenating successive odometry
  // readings-
  double odomVector[3] = { odometry->pose.pose.position.x,
                           odometry->pose.pose.position.y,
                           tf::getYaw(odometry->pose.pose.orientation) };

  Eigen::Isometry2d odom;
  odom = Eigen::Rotation2Dd(odomVector[2]).toRotationMatrix();
  odom.translation() = Eigen::Vector2d(odomVector[0], odomVector[1]);

  if (seq == 0)
    curPose_ = initPose_;
  else
  {
    prevPose_ = curPose_;
    curPose_ = prevPose_ * odom;
  }

  Eigen::Vector2d t;
  t = curPose_.translation();
  Eigen::Matrix<double, 2, 2> r = curPose_.linear();
  double angle = acos(r(0, 0));

  if (parent_->areAllRobotsActive() && !particlesInitialized)
  {
    initPFset();
    particlesInitialized = true;
  }

  // particle prediction step
  for (int i = 0; i < N_PARTICLES; i++)
  {
    Eigen::Isometry2d prevParticle, curParticle;

    if (seq != 0)
    {
      prevParticle =
          Eigen::Rotation2Dd(particleSet_[2 + (RobotNumber - 1) * 3][i])
          .toRotationMatrix();
      prevParticle.translation() =
          Eigen::Vector2d(particleSet_[0 + (RobotNumber - 1) * 3][i],
          particleSet_[1 + (RobotNumber - 1) * 3][i]);
      curParticle = prevParticle * odom;
      Eigen::Vector2d t = curParticle.translation();
      particleSet_[0 + (RobotNumber - 1) * 3][i] = t(0);
      particleSet_[1 + (RobotNumber - 1) * 3][i] = t(1);
      Eigen::Matrix<double, 2, 2> r_particle = curParticle.linear();
      double angle_particle = acos(r_particle(0, 0));
      particleSet_[2 + (RobotNumber - 1) * 3][i] = angle_particle;

      for (int j = 0; j < (MAX_ROBOTS + 1) * 3 + 1; j++)
      {
        // particleSet_[j][i] = pfParticlesSelf[i][j];
        // pfucltPtcls.particles[i].particle[j] = particleSet_[j][i];
      }
    }
  }

  //   publishState(0,0,0);
  // cout<<"just after publishing state"<<endl;
  // ROS_INFO("Odometry propagated self robot state is x=%f, y=%f,
  // theta=%f",t(0),t(1),angle);
}

void SelfRobot::selfTargetDataCallback(
    const read_omni_dataset::BallData::ConstPtr& ballData, uint RobotNumber)
{
  // ROS_INFO("Got ball data from self robot %d",RobotNumber);
  ros::Time curObservationTime = ballData->header.stamp;

  if (ballData->found)
  {
    /// Below is the procedure to calculate the observation covariance associate
    /// with the ball measurement made by the robots. Caution: Make sure the
    /// validity of the calculations below by crosschecking the obvious things,
    /// e.g., covariance cannot be negative or very close to 0

    Eigen::Vector2d tempBallObsVec = Eigen::Vector2d(ballData->x, ballData->y);

    double d = tempBallObsVec.norm(), phi = atan2(ballData->y, ballData->x);

    double covDD =
        (double)(1 / ballData->mismatchFactor) * (K3 * d + K4 * (d * d));
    double covPhiPhi = K5 * (1 / (d + 1));

    double covXX =
        pow(cos(phi), 2) * covDD +
        pow(sin(phi), 2) * (pow(d, 2) * covPhiPhi + covDD * covPhiPhi);
    double covYY =
        pow(sin(phi), 2) * covDD +
        pow(cos(phi), 2) * (pow(d, 2) * covPhiPhi + covDD * covPhiPhi);
    // ROS_INFO("Ball found in the image, refer to the method to see how
    // covariances are calculated");
  }
  else
  {
    // ROS_INFO("Ball not found in the image");
  }
}

void SelfRobot::selfLandmarkDataCallback(
    const read_omni_dataset::LRMLandmarksData::ConstPtr& landmarkData,
    uint RobotNumber)
{
  //   return;
  // ROS_INFO(" got landmark data from self robot (ID=%d)",RobotNumber);
  bool landmarkfound[10];
  {
    float d_0 =
        pow((pow(landmarkData->x[0], 2) + pow(landmarkData->y[0], 2)), 0.5);
    float d_1 =
        pow((pow(landmarkData->x[1], 2) + pow(landmarkData->y[1], 2)), 0.5);
    float d_2 =
        pow((pow(landmarkData->x[2], 2) + pow(landmarkData->y[2], 2)), 0.5);
    float d_3 =
        pow((pow(landmarkData->x[3], 2) + pow(landmarkData->y[3], 2)), 0.5);
    float d_4 =
        pow((pow(landmarkData->x[4], 2) + pow(landmarkData->y[4], 2)), 0.5);
    float d_5 =
        pow((pow(landmarkData->x[5], 2) + pow(landmarkData->y[5], 2)), 0.5);
    float d_6 =
        pow((pow(landmarkData->x[6], 2) + pow(landmarkData->y[6], 2)), 0.5);
    float d_7 =
        pow((pow(landmarkData->x[7], 2) + pow(landmarkData->y[7], 2)), 0.5);
    float d_8 =
        pow((pow(landmarkData->x[8], 2) + pow(landmarkData->y[8], 2)), 0.5);
    float d_9 =
        pow((pow(landmarkData->x[9], 2) + pow(landmarkData->y[9], 2)), 0.5);

    for (int i = 0; i < 10; i++)
    {
      landmarkfound[i] = landmarkData->found[i];
    }

    // Huristic 1. If I see only 8 and not 9.... then I cannot see 7
    if (landmarkData->found[8] && !landmarkData->found[9])
    {
      landmarkfound[7] = false;
    }

    // Huristic 2. If I see only 9 and not 8.... then I cannot see 6
    if (!landmarkData->found[8] && landmarkData->found[9])
    {
      landmarkfound[6] = false;
    }

    // Huristic 3. If I see both 8 and 9.... then there are 2 subcases
    if (landmarkData->found[8] && landmarkData->found[9])
    {
      // Huristic 3.1. If I am closer to 9.... then I cannot see 6
      if (d_9 < d_8)
      {
        landmarkfound[6] = false;
      }
      // Huristic 3.2 If I am closer to 8.... then I cannot see 7
      if (d_8 < d_9)
      {
        landmarkfound[7] = false;
      }
    }

    float lm_4_thresh, lm_5_thresh, lm_8_thresh, lm_9_thresh;

    if (RobotNumber == 4)
    {
      lm_4_thresh = 3.0;
      lm_5_thresh = 3.0;
      lm_8_thresh = 3.0;
      lm_9_thresh = 3.0;
    }

    if (RobotNumber == 3)
    {
      lm_4_thresh = 6.5;
      lm_5_thresh = 6.5;
      lm_8_thresh = 6.5;
      lm_9_thresh = 6.5;
    }

    if (RobotNumber == 1)
    {
      lm_4_thresh = 6.5;
      lm_5_thresh = 6.5;
      lm_8_thresh = 6.5;
      lm_9_thresh = 6.5;
    }

    if (RobotNumber == 5)
    {
      lm_4_thresh = 3.5;
      lm_5_thresh = 3.5;
      lm_8_thresh = 3.5;
      lm_9_thresh = 3.5;
    }

    // Huristic 4. Additional huristic for landmark index 6.... if I observe it
    // further than 4m then nullify it
    if (d_6 > 3.5)
    {
      landmarkfound[6] = false;
    }

    // Huristic 5. Additional huristic for landmark index 7.... if I observe it
    // further than 4m then nullify it
    if (d_7 > 3.5)
    {
      landmarkfound[7] = false;
    }

    // Huristic 6. Additional huristic for landmark index 8.... if I observe it
    // further than 4m then nullify it
    if (d_8 > lm_8_thresh)
    {
      landmarkfound[8] = false;
    }

    // Huristic 7. Additional huristic for landmark index 9.... if I observe it
    // further than 4m then nullify it
    if (d_9 > lm_9_thresh)
    {
      landmarkfound[9] = false;
    }

    // Huristic 8. Additional huristic for landmark index 4.... if I observe it
    // further than 4m then nullify it
    if (d_4 > lm_4_thresh)
    {
      landmarkfound[4] = false;
    }

    // Huristic 9. Additional huristic for landmark index 5.... if I observe it
    // further than 4m then nullify it
    if (d_5 > lm_5_thresh)
    {
      landmarkfound[5] = false;
    }

    // Huristic 10. Additional huristic for landmark index 0.... if I observe it
    // further than 4m then nullify it
    if (d_0 > 2.5)
    {
      landmarkfound[0] = false;
    }

    // Huristic 11. Additional huristic for landmark index 1.... if I observe it
    // further than 4m then nullify it
    if (d_1 > 2.5)
    {
      landmarkfound[1] = false;
    }

    // Huristic 12. Additional huristic for landmark index 2.... if I observe it
    // further than 4m then nullify it
    if (d_2 > 2.5)
    {
      landmarkfound[2] = false;
    }

    // Huristic 13. Additional huristic for landmark index 3.... if I observe it
    // further than 4m then nullify it
    if (d_3 > 2.5)
    {
      landmarkfound[3] = false;
    }
  }

  uint seq = landmarkData->header.seq;

  // There are a total of 10 distinct, known and static landmarks in this
  // dataset.

  // float weight[nParticles_];
  for (int p = 0; p < N_PARTICLES; p++)
  {
    pfParticles_[(MAX_ROBOTS + 1) * 3][p] = 1.0;
    particleSet_[(MAX_ROBOTS + 1) * 3][p] = 1.0;
  }
  // cout<<"Particle weights are = ";

  for (int i = 0; i < 10; i++)
  {
    if (landmarkfound[i])
    {

      /// Below is the procedure to calculate the observation covariance
      /// associate with the ball measurement made by the robots. Caution: Make
      /// sure the validity of the calculations below by crosschecking the
      /// obvious things, e.g., covariance cannot be negative or very close to 0

      Eigen::Vector2d tempLandmarkObsVec =
          Eigen::Vector2d(landmarkData->x[i], landmarkData->y[i]);

      double d = tempLandmarkObsVec.norm(),
          phi = atan2(landmarkData->y[i], landmarkData->x[i]);

      double covDD =
          (K1 * fabs(1.0 - (landmarkData->AreaLandMarkActualinPixels[i] /
                            landmarkData->AreaLandMarkExpectedinPixels[i]))) *
          (d * d);
      double covPhiPhi = 10 * K2 * (1 / (d + 1));

      double covXX =
          pow(cos(phi), 2) * covDD +
          pow(sin(phi), 2) * (pow(d, 2) * covPhiPhi + covDD * covPhiPhi);
      double covYY =
          pow(sin(phi), 2) * covDD +
          pow(cos(phi), 2) * (pow(d, 2) * covPhiPhi + covDD * covPhiPhi);

      for (int p = 0; p < N_PARTICLES; p++)
      {

        // 	  float ObsWorldLM_X = particleSet_[0+(RobotNumber-1)*3][p] +
        // landmarkData->x[i]* cos(particleSet_[2+(RobotNumber-1)*3][p]) -
        // landmarkData->y[i]*sin(particleSet_[2+(RobotNumber-1)*3][p]);
        // 	  float ObsWorldLM_Y = particleSet_[1+(RobotNumber-1)*3][p] +
        // landmarkData->x[i]* sin(particleSet_[2+(RobotNumber-1)*3][p]) +
        // landmarkData->y[i]*cos(particleSet_[2+(RobotNumber-1)*3][p]);
        //
        //
        // 	  float error = powf( (powf((landmarks[i][0] - ObsWorldLM_X),2)
        // +
        // powf((landmarks[i][1] - ObsWorldLM_Y),2)) , 0.5 );
        //
        // 	  if(error>1.0)
        // 	  {
        // 	    cout<<"Landmark "<<i+1<<" observation at timestep
        // "<<landmarkData->header.stamp <<" is at X = "<<landmarkData->x[i]<<"
        // at Y = "<<landmarkData->y[i]<<" covXX = "<<covXX<<" covYY =
        // "<<covYY<<endl;
        //
        // 	    cout<<"Landmark "<<i+1<<" actuall is at X =
        // "<<landmarks[i][0]<<" at Y = "<<landmarks[i][1]<<endl<<endl;
        // 	  }

        // More formal method is this
        float Z[2], Zcap[2], Q[2][2], Q_inv[2][2], Z_Zcap[2];

        Z[0] = landmarkData->x[i];
        Z[1] = landmarkData->y[i];

        Zcap[0] =
            (landmarks[i].x - particleSet_[0 + (RobotNumber - 1) * 3][p]) *
            (cos(particleSet_[2 + (RobotNumber - 1) * 3][p])) +
            (landmarks[i].y - particleSet_[1 + (RobotNumber - 1) * 3][p]) *
            (sin(particleSet_[2 + (RobotNumber - 1) * 3][p]));

        Zcap[1] =
            -(landmarks[i].x - particleSet_[0 + (RobotNumber - 1) * 3][p]) *
            (sin(particleSet_[2 + (RobotNumber - 1) * 3][p])) +
            (landmarks[i].y - particleSet_[1 + (RobotNumber - 1) * 3][p]) *
            (cos(particleSet_[2 + (RobotNumber - 1) * 3][p]));

        Z_Zcap[0] = Z[0] - Zcap[0];
        Z_Zcap[1] = Z[1] - Zcap[1];

        Q[0][0] = covXX;
        Q[0][1] = 0.0;
        Q[1][0] = 0.0;
        Q[1][1] = covYY;

        Q_inv[0][0] = 1 / covXX;
        Q_inv[0][1] = 0.0;
        Q_inv[1][0] = 0.0;
        Q_inv[1][1] = 1 / covYY;
        float ExpArg = -0.5 * (Z_Zcap[0] * Z_Zcap[0] * Q_inv[0][0] +
            Z_Zcap[1] * Z_Zcap[1] * Q_inv[1][1]);
        float detValue = powf((2 * PI * Q[0][0] * Q[1][1]), -0.5);

        // cout<<"weight of particle at x =
        // "<<particleSet_[0+(RobotNumber-1)*3][p]<<" , Y =
        // "<<particleSet_[1+(RobotNumber-1)*3][p]<<" and orientation =
        // "<<particleSet_[2+(RobotNumber-1)*3][p] <<" has ExpArg = "
        // <<ExpArg<<endl;

        particleSet_[(MAX_ROBOTS + 1) * 3][p] =
            particleSet_[(MAX_ROBOTS + 1) * 3][p] + 1 * exp(ExpArg);
        // particleSet_[(MAX_ROBOTS+1)*3][p] =
        // pfParticlesSelf[p][(MAX_ROBOTS+1)*3];
      }
      // ROS_INFO("Landmark %d found in the image, refer to the method to see
      // how covariances are calculated",i);
    }
  }

  // if(seq%30==0)
  PFresample();
  //
  //     for(int i=0; i<nParticles_; i++)
  //     {
  //       cout<<"weight of particle at x =
  //       "<<particleSet_[0+(RobotNumber-1)*3][i]<<" , Y =
  //       "<<particleSet_[1+(RobotNumber-1)*3][i]<<" and orientation =
  //       "<<particleSet_[2+(RobotNumber-1)*3][i] <<" has weight = "
  //       <<particleSet_[(MAX_ROBOTS+1)*3][i]<<endl;
  //     }
  // cout<<"just before publishing state"<<endl;
  publishState(0, 0, 0);
}

void SelfRobot::gtDataCallback(
    const read_omni_dataset::LRMGTData::ConstPtr& gtMsgReceived)
{
  receivedGTdata = *gtMsgReceived;
}

////////////////////////// METHOD DEFINITIONS OF THE TEAMMATEROBOT CLASS
/////////////////////////

TeammateRobot::TeammateRobot(ros::NodeHandle& nh, Eigen::Isometry2d initPose,
                             particles& ptcls, RobotFactory* caller,
                             uint robotNumber)
  : Robot(nh, caller, initPose, ptcls, robotNumber)
{

  sOdom_ = nh.subscribe<nav_msgs::Odometry>(
        "/omni" + boost::lexical_cast<std::string>(robotNumber + 1) + "/odometry",
        10, boost::bind(&TeammateRobot::teammateOdometryCallback, this, _1,
                        robotNumber + 1));

  sBall_ = nh.subscribe<read_omni_dataset::BallData>(
        "/omni" + boost::lexical_cast<std::string>(robotNumber + 1) +
        "/orangeball3Dposition",
        10, boost::bind(&TeammateRobot::teammateTargetDataCallback, this, _1,
                        robotNumber + 1));

  sLandmark_ = nh.subscribe<read_omni_dataset::LRMLandmarksData>(
        "/omni" + boost::lexical_cast<std::string>(robotNumber + 1) +
        "/landmarkspositions",
        10, boost::bind(&TeammateRobot::teammateLandmarkDataCallback, this, _1,
                        robotNumber + 1));

  ROS_INFO(" constructing TeammateRobot object and called sensor subscribers "
           "for robot %d",
           robotNumber + 1);

  // Store pointer to caller
  parent_ = caller;

  // Mark initialized flags as false
  setStarted(false);
}

void TeammateRobot::teammateOdometryCallback(
    const nav_msgs::Odometry::ConstPtr& odometry, uint RobotNumber)
{

  setStarted(true);

  uint seq = odometry->header.seq;
  prevTime_ = curTime_;
  curTime_ = odometry->header.stamp;

  // ROS_INFO(" got odometry from teammate-robot (ID=%d) at time
  // %d\n",RobotNumber, odometry->header.stamp.sec);

  // Below is an example how to extract the odometry from the message and use it
  // to propogate the robot state by simply concatenating successive odometry
  // readings-
  double odomVector[3] = { odometry->pose.pose.position.x,
                           odometry->pose.pose.position.y,
                           tf::getYaw(odometry->pose.pose.orientation) };

  Eigen::Isometry2d odom;
  odom = Eigen::Rotation2Dd(odomVector[2]).toRotationMatrix();
  odom.translation() = Eigen::Vector2d(odomVector[0], odomVector[1]);

  if (seq == 0)
    curPose_ = initPose_;
  else
  {
    prevPose_ = curPose_;
    curPose_ = prevPose_ * odom;
  }

  Eigen::Vector2d t;
  t = curPose_.translation();
  Eigen::Matrix<double, 2, 2> r = curPose_.linear();
  double angle = acos(r(0, 0));

  // particle prediction step
  for (int i = 0; i < N_PARTICLES; i++)
  {
    Eigen::Isometry2d prevParticle, curParticle;

    if (seq != 0)
    {
      prevParticle =
          Eigen::Rotation2Dd(pfParticles_[2 + (RobotNumber - 1) * 3][i])
          .toRotationMatrix();
      prevParticle.translation() =
          Eigen::Vector2d(pfParticles_[0 + (RobotNumber - 1) * 3][i],
          pfParticles_[1 + (RobotNumber - 1) * 3][i]);
      curParticle = prevParticle * odom;
      pfParticles_[0 + (RobotNumber - 1) * 3][i] = curParticle.translation()[0];
      pfParticles_[1 + (RobotNumber - 1) * 3][i] = curParticle.translation()[1];
      Eigen::Matrix<double, 2, 2> r_particle = curParticle.linear();
      double angle_particle = acos(r_particle(0, 0));
      pfParticles_[2 + (RobotNumber - 1) * 3][i] = angle_particle;
    }
  }

  // ROS_INFO("Odometry propogated state f robot OMNI%d is x=%f, y=%f,
  // theta=%f",RobotNumber, t(0),t(1),angle);
}

void TeammateRobot::teammateTargetDataCallback(
    const read_omni_dataset::BallData::ConstPtr& ballData, uint RobotNumber)
{
  // ROS_INFO("Got ball data from teammate robot %d",RobotNumber);
  ros::Time curObservationTime = ballData->header.stamp;

  if (ballData->found)
  {
    /// Below is the procedure to calculate the observation covariance associate
    /// with the ball measurement made by the robots.

    Eigen::Vector2d tempBallObsVec = Eigen::Vector2d(ballData->x, ballData->y);

    double d = tempBallObsVec.norm(), phi = atan2(ballData->y, ballData->x);

    double covDD =
        (double)(1 / ballData->mismatchFactor) * (K3 * d + K4 * (d * d));
    double covPhiPhi = K5 * (1 / (d + 1));

    double covXX =
        pow(cos(phi), 2) * covDD +
        pow(sin(phi), 2) * (pow(d, 2) * covPhiPhi + covDD * covPhiPhi);
    double covYY =
        pow(sin(phi), 2) * covDD +
        pow(cos(phi), 2) * (pow(d, 2) * covPhiPhi + covDD * covPhiPhi);
    // ROS_INFO("Ball found in the image, refer to the method to see how
    // covariances are calculated");
  }
  else
  {
    // ROS_WARN("Ball not found in the image");
  }
}

void TeammateRobot::teammateLandmarkDataCallback(
    const read_omni_dataset::LRMLandmarksData::ConstPtr& landmarkData,
    uint robotNumber)
{
  // ROS_INFO(" got landmark data from teammate robot (ID=%d)",RobotNumber);

  uint seq = landmarkData->header.seq;

  // There are a total of 10 distinct, known and static landmarks in this
  // dataset.
  for (int i = 0; i < 10; i++)
  {
    if (landmarkData->found[i])
    {

      /// Below is the procedure to calculate the observation covariance
      /// associate with the ball measurement made by the robots. Caution: Make
      /// sure the validity of the calculations below by crosschecking the
      /// obvious things, e.g., covariance cannot be negative or very close to 0

      Eigen::Vector2d tempLandmarkObsVec =
          Eigen::Vector2d(landmarkData->x[i], landmarkData->y[i]);

      double d = tempLandmarkObsVec.norm(),
          phi = atan2(landmarkData->y[i], landmarkData->x[i]);

      double covDD =
          (K1 * fabs(1.0 - (landmarkData->AreaLandMarkActualinPixels[i] /
                            landmarkData->AreaLandMarkExpectedinPixels[i]))) *
          (d * d);
      double covPhiPhi = K2 * (1 / (d + 1));

      double covXX =
          pow(cos(phi), 2) * covDD +
          pow(sin(phi), 2) * (pow(d, 2) * covPhiPhi + covDD * covPhiPhi);
      double covYY =
          pow(sin(phi), 2) * covDD +
          pow(cos(phi), 2) * (pow(d, 2) * covPhiPhi + covDD * covPhiPhi);

      // ROS_INFO("Landmark %d found in the image, refer to the method to see
      // how covariances are calculated",i);
    }
  }
}

void SelfRobot::publishState(float x, float y, float theta)
{
  // using the first robot for the globaltime stamp of this message
  // printf("we are here \n\n\n");

  // first publish the particles!!!!
  particlePublisher.publish(msg_particles);

  msg_state.header.stamp =
      curTime_; // time of the self-robot must be in the full state
  receivedGTdata.header.stamp = curTime_;

  msg_state.robotPose[MY_ID - 1].pose.pose.position.x = x;
  msg_state.robotPose[MY_ID - 1].pose.pose.position.y = y;
  msg_state.robotPose[MY_ID - 1].pose.pose.position.z =
      ROB_HT; // fixed height aboveground

  msg_state.robotPose[MY_ID - 1].pose.pose.orientation.x = 0;
  msg_state.robotPose[MY_ID - 1].pose.pose.orientation.y = 0;
  msg_state.robotPose[MY_ID - 1].pose.pose.orientation.z = sin(theta / 2);
  msg_state.robotPose[MY_ID - 1].pose.pose.orientation.w = cos(theta / 2);

  State_publisher.publish(msg_state);
  virtualGTPublisher.publish(receivedGTdata);
}

// end of namespace
}

int main(int argc, char* argv[])
{

  ros::init(argc, argv, "pfuclt_omni_dataset");
  ros::NodeHandle nh;

#ifdef DEBUG
  if (ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME,
                                     ros::console::levels::Debug))
  {
    ros::console::notifyLoggerLevelsChanged();
  }
#endif

  // read parameters from param server
  using pfuclt_aux::readParam;
  using namespace pfuclt;

  readParam<int>(nh, "/MAX_ROBOTS", MAX_ROBOTS);
  readParam<int>(nh, "/NUM_ROBOTS", NUM_ROBOTS);
  readParam<float>(nh, "/ROB_HT", ROB_HT);
  readParam<int>(nh, "/MY_ID", MY_ID);
  readParam<int>(nh, "/N_PARTICLES", N_PARTICLES);
  readParam<int>(nh, "/NUM_SENSORS_PER_ROBOT", NUM_SENSORS_PER_ROBOT);
  readParam<int>(nh, "/NUM_TARGETS", NUM_TARGETS);
  readParam<float>(nh, "/LANDMARK_COV/K1", K1);
  readParam<float>(nh, "/LANDMARK_COV/K2", K2);
  readParam<float>(nh, "/LANDMARK_COV/K3", K3);
  readParam<float>(nh, "/LANDMARK_COV/K4", K4);
  readParam<float>(nh, "/LANDMARK_COV/K5", K5);
  readParam<bool>(nh, "/PLAYING_ROBOTS", PLAYING_ROBOTS);
  readParam<double>(nh, "/POS_INIT", POS_INIT);

  N_DIMENSIONS = (MAX_ROBOTS + NUM_TARGETS) * STATES_PER_ROBOT + NUM_WEIGHT;

  if (N_PARTICLES < 0 || N_DIMENSIONS < 0)
  {
    ROS_ERROR("Unacceptable configuration for particles class");
    nh.shutdown();
    return 0;
  }

  pfuclt::RobotFactory Factory(nh);

  if (pfuclt::MY_ID == 2)
  {
    // ROS_WARN("OMNI2 not present in dataset. Please try with another Robot ID
    // for self robot");
    return 0;
  }

  Factory.initializeFixedLandmarks();

  ros::spin();
  return 0;
}
