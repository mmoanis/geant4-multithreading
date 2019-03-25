#include "MyWorkerRunManager.hpp"
#include <G4WorkerRunManagerKernel.hh>
#include <G4UImanager.hh>
#include <G4VUserDetectorConstruction.hh>
#include "MyRunManager.hpp"
#include <G4ScoringManager.hh>
#include <G4TransportationManager.hh>
#include <G4ParallelWorldProcess.hh>
#include <G4WorkerThread.hh>
#include <G4VUserPhysicsList.hh>
#include <G4VUserActionInitialization.hh>
#include <G4UserWorkerInitialization.hh>
#include <G4UserWorkerThreadInitialization.hh>
#include <G4UserRunAction.hh>
#include <G4RNGHelper.hh>
#include <G4Run.hh>
#include <G4VUserPrimaryGeneratorAction.hh>
#include <G4VVisManager.hh>
#include <G4SDManager.hh>
#include <G4VScoreNtupleWriter.hh>
#include <G4VScoringMesh.hh>
#include <G4Timer.hh>
#include <G4TiMemory.hh>
#include <sstream>
#include <fstream>

MyWorkerRunManager* MyWorkerRunManager::GetWorkerRunManager()
{ return static_cast<MyWorkerRunManager*>(G4RunManager::GetRunManager()); }

G4WorkerRunManagerKernel* MyWorkerRunManager::GetWorkerRunManagerKernel()
{ return static_cast<G4WorkerRunManagerKernel*>(GetWorkerRunManager()->kernel); }

MyWorkerRunManager::MyWorkerRunManager() : G4RunManager(workerRM) {
    //This constructor should never be called in non-multithreaded mode
#ifndef G4MULTITHREADED
    G4ExceptionDescription msg;
    msg<<"Geant4 code is compiled without multi-threading support (-DG4MULTITHREADED is set to off).";
    msg<<" This type of RunManager can only be used in mult-threaded applications.";
    G4Exception("MyWorkerRunManager::MyWorkerRunManager()","Run0103",FatalException,msg);
#endif
    G4ParticleTable::GetParticleTable()->WorkerG4ParticleTable();
    G4ScoringManager* masterScM = MyRunManager::GetMasterScoringManager();
    if(masterScM) G4ScoringManager::GetScoringManager(); //TLS instance for a worker

    eventLoopOnGoing = false;
    runIsSeeded = false;
    nevModulo = -1;
    currEvID = -1;
    workerContext = 0;
    readStatusFromFile = false;

    G4UImanager::GetUIpointer()->SetIgnoreCmdNotFound(true);

    G4VVisManager* pVVis = G4VVisManager::GetConcreteInstance();
    if(pVVis)
    {
      pVVis->SetUpForAThread();
      visIsSetUp = true;
    }
    else
    { visIsSetUp = false; }

}

#include "MyRunManager.hpp"

MyWorkerRunManager::~MyWorkerRunManager() {
    // Delete thread-local data process manager objects
    physicsList->TerminateWorker();
//    physicsList->RemoveProcessManager();

    //Put these pointers to zero: owned by master thread
    //If not to zero, the base class destructor will attempt to
    //delete them
    userDetector = 0;
    userWorkerInitialization = 0;
    userWorkerThreadInitialization = 0;
    userActionInitialization = 0;
    physicsList = 0;
    if(verboseLevel>0) G4cout<<"Destroying WorkerRunManager ("<<this<<")"<<G4endl;
}


void MyWorkerRunManager::InitializeGeometry() {
  std::cout << "InitializeGeometry " <<  this << std::endl;
    if(!userDetector)
    {
        G4Exception("G4RunManager::InitializeGeometry", "Run0033",
                    FatalException, "G4VUserDetectorConstruction is not defined!");
        return;
    }
    if(fGeometryHasBeenDestroyed) 
    { G4TransportationManager::GetTransportationManager()->ClearParallelWorlds(); }

    //Step1: Get pointer to the physiWorld (note: needs to get the "super pointer, i.e. the one shared by all threads"
    G4RunManagerKernel* masterKernel = MyRunManager::GetMasterRunManagerKernel();
    G4VPhysicalVolume* worldVol = masterKernel->GetCurrentWorld();
    //Step2:, Call a new "WorkerDefineWorldVolume( pointer from 2-, false); 
    kernel->WorkerDefineWorldVolume(worldVol,false);
    kernel->SetNumberOfParallelWorld(masterKernel->GetNumberOfParallelWorld());
    //Step3: Call user's ConstructSDandField()
    userDetector->ConstructSDandField();
    userDetector->ConstructParallelSD();
    geometryInitialized = true;
}

void MyWorkerRunManager::InitializePhysics() {
    std::cout << "InitializePhysics " <<  this << std::endl;
  G4RunManager::InitializePhysics();
}

#include <G4ParallelWorldProcessStore.hh>

void MyWorkerRunManager::RunInitialization()
{
    TIMEMORY_AUTO_TIMER("");
  if(!visIsSetUp)
  {
    G4VVisManager* pVVis = G4VVisManager::GetConcreteInstance();
    if(pVVis)
    {
      pVVis->SetUpForAThread();
      visIsSetUp = true;
    }
  }

  if(!(kernel->RunInitialization(fakeRun))) return;

  //Signal this thread can start event loop.
  //Note this will return only when all threads reach this point
  MyRunManager::GetMasterRunManager()->ThisWorkerReady();
  if(fakeRun) return;

  const G4UserWorkerInitialization* uwi
       = MyRunManager::GetMasterRunManager()->GetUserWorkerInitialization();
  CleanUpPreviousEvents();
  if(currentRun) delete currentRun;
  currentRun = 0;

  if(fGeometryHasBeenDestroyed) G4ParallelWorldProcessStore::GetInstance()->UpdateWorlds();
  //Call a user hook: this is guaranteed all threads are "synchronized"
  if(uwi) uwi->WorkerRunStart();

  if(userRunAction) currentRun = userRunAction->GenerateRun();
  if(!currentRun) currentRun = new G4Run();

  currentRun->SetRunID(runIDCounter);
  currentRun->SetNumberOfEventToBeProcessed(numberOfEventToBeProcessed);

  currentRun->SetDCtable(DCtable);
  G4SDManager* fSDM = G4SDManager::GetSDMpointerIfExist();
  if(fSDM)
  { currentRun->SetHCtable(fSDM->GetHCtable()); }

  if ( G4VScoreNtupleWriter::Instance() )
  {
    auto hce = fSDM->PrepareNewEvent();
    isScoreNtupleWriter = G4VScoreNtupleWriter::Instance()->Book(hce);
    delete hce;
  }

  std::ostringstream oss;
    G4Random::saveFullState(oss);
  randomNumberStatusForThisRun = oss.str();
  currentRun->SetRandomNumberStatus(randomNumberStatusForThisRun);

  for(G4int i_prev=0;i_prev<n_perviousEventsToBeStored;i_prev++)
  { previousEvents->push_back((G4Event*)0); }

  if(printModulo>0 || verboseLevel>0)
  {
    G4cout << "### Run " << currentRun->GetRunID() << " starts on worker thread "
           << G4Threading::G4GetThreadId() << "." << G4endl;
  }
  if(userRunAction) userRunAction->BeginOfRunAction(currentRun);

  if (isScoreNtupleWriter)  {
    G4VScoreNtupleWriter::Instance()->OpenFile();
  }

  if(storeRandomNumberStatus) {
      G4String fileN = "currentRun";
      if ( rngStatusEventsFlag ) {
          std::ostringstream os;
          os << "run" << currentRun->GetRunID();
          fileN = os.str();
      }
      StoreRNGStatus(fileN);
  }

  runAborted = false;
  numberOfEventProcessed = 0;
}

void MyWorkerRunManager::DoEventLoop(G4int n_event, const char* macroFile , G4int n_select)
{
    TIMEMORY_AUTO_TIMER("");
    if(!userPrimaryGeneratorAction)
    {
      G4Exception("G4RunManager::GenerateEvent()", "Run0032", FatalException,
                "G4VUserPrimaryGeneratorAction is not defined!");
    }

    //This is the same as in the sequential case, just the for-loop indexes are
    //different
    InitializeEventLoop(n_event,macroFile,n_select);

    // Reset random number seeds queue
    while(seedsQueue.size()>0)
    { seedsQueue.pop(); }
    // for each run, worker should receive at least one set of random number seeds.
    runIsSeeded = false; 

    // Event loop
    eventLoopOnGoing = true;
///////    G4int i_event = workerContext->GetThreadId();
    G4int i_event = -1;
    nevModulo = -1;
    currEvID = -1;

    while(eventLoopOnGoing)
    {
      ProcessOneEvent(i_event);
      if(eventLoopOnGoing)
      {
        TerminateOneEvent();
        if(runAborted)
        { eventLoopOnGoing = false; }
//////        else
//////        {
//////          i_event += workerContext->GetNumberThreads();
//////          eventLoopOnGoing = i_event<n_event;
//////        }
      }
    }
     
    TerminateEventLoop();
}

void MyWorkerRunManager::ProcessOneEvent(G4int i_event)
{
    TIMEMORY_AUTO_TIMER("");
  currentEvent = GenerateEvent(i_event);
  if(eventLoopOnGoing)
  {  
    eventManager->ProcessOneEvent(currentEvent);
    AnalyzeEvent(currentEvent);
    UpdateScoring();
    if(currentEvent->GetEventID()<n_select_msg) G4UImanager::GetUIpointer()->ApplyCommand(msgText);
  }
}

G4Event* MyWorkerRunManager::GenerateEvent(G4int i_event)
{
    TIMEMORY_AUTO_TIMER("");
  G4Event* anEvent = new G4Event(i_event);
  long s1 = 0;
  long s2 = 0;
  long s3 = 0;
  G4bool eventHasToBeSeeded = true;
  if(MyRunManager::SeedOncePerCommunication()==1 && runIsSeeded)
  { eventHasToBeSeeded = false; }

  if(i_event<0)
  {
    G4int nevM = MyRunManager::GetMasterRunManager()->GetEventModulo();
    if(nevM==1)
    {
      eventLoopOnGoing = MyRunManager::GetMasterRunManager()
                       ->SetUpAnEvent(anEvent,s1,s2,s3,eventHasToBeSeeded);
      runIsSeeded = true;
    }
    else
    {
      if(nevModulo<=0)
      {
        G4int nevToDo = MyRunManager::GetMasterRunManager()
                         ->SetUpNEvents(anEvent,&seedsQueue,eventHasToBeSeeded);
        if(nevToDo==0)
        { eventLoopOnGoing = false; }
        else
        {
          currEvID = anEvent->GetEventID();
          nevModulo = nevToDo - 1;
        }
      }
      else
      {
        if(MyRunManager::SeedOncePerCommunication()>0) eventHasToBeSeeded = false;
        anEvent->SetEventID(++currEvID);
        nevModulo--;
      }
      if(eventLoopOnGoing && eventHasToBeSeeded)
      {
        s1 = seedsQueue.front(); seedsQueue.pop();
        s2 = seedsQueue.front(); seedsQueue.pop();
      }
    }

    if(!eventLoopOnGoing)
    {
      delete anEvent;
      return 0;
    }
  }
  else if(eventHasToBeSeeded)
  {
    //Need to reseed random number generator
    G4RNGHelper* helper = G4RNGHelper::GetInstance();
    s1 = helper->GetSeed(i_event*2);
    s2 = helper->GetSeed(i_event*2+1);
  }

  if(eventHasToBeSeeded) 
  {
    long seeds[3] = { s1, s2, 0 };
    G4Random::setTheSeeds(seeds,-1);
    runIsSeeded = true;
////G4cout<<"Event "<<currEvID<<" is seeded with { "<<s1<<", "<<s2<<" }"<<G4endl;
  }

  //Read from file seed.
  //Andrea Dotti 4 November 2015
  //This is required for strong-reproducibility, in MT mode we have that each
  //thread produces, for each event a status file, we want to do that.
  //Search a random file with the format run{%d}evt{%d}.rndm

  //This is the filename base constructed from run and event
  const auto filename = [&] {
	  std::ostringstream os;
	  os << "run"<<currentRun->GetRunID() << "evt" << anEvent->GetEventID();
	  return os.str();
  };

  G4bool RNGstatusReadFromFile = false;
 if ( readStatusFromFile ) {
	 //Build full path of RNG status file for this event
	 std::ostringstream os;
	 os << filename() << ".rndm";
	 const G4String& randomStatusFile = os.str();
	 std::ifstream ifile(randomStatusFile.c_str());
	 if ( ifile ) { //File valid and readable
		 RNGstatusReadFromFile = true;
		 G4Random::restoreEngineStatus(randomStatusFile.c_str());
	 }
 }


  if(storeRandomNumberStatusToG4Event==1 || storeRandomNumberStatusToG4Event==3)
  {
    std::ostringstream oss;
    G4Random::saveFullState(oss);
    randomNumberStatusForThisEvent = oss.str();
    anEvent->SetRandomNumberStatus(randomNumberStatusForThisEvent);
  }

  if(storeRandomNumberStatus && ! RNGstatusReadFromFile ) { //If reading from file, avoid to rewrite the same
      G4String fileN = "currentEvent";
      if ( rngStatusEventsFlag ) {
    	  fileN = filename();
      }
      StoreRNGStatus(fileN);
  }

  if(printModulo > 0 && anEvent->GetEventID()%printModulo == 0 )
  {
    G4cout << "--> Event " << anEvent->GetEventID() << " starts";
    if(eventHasToBeSeeded)
    { G4cout << " with initial seeds (" << s1 << "," << s2 << ")"; }
    G4cout << "." << G4endl;
  }
  userPrimaryGeneratorAction->GeneratePrimaries(anEvent);
  return anEvent;
}

void MyWorkerRunManager::MergePartialResults()
{
    //Merge partial results into global run
    MyRunManager* mtRM = MyRunManager::GetMasterRunManager();
    G4ScoringManager* ScM = G4ScoringManager::GetScoringManagerIfExist();
    if(ScM) mtRM->MergeScores(ScM);
    mtRM->MergeRun(currentRun);
}

void MyWorkerRunManager::RunTermination()
{
  if(!fakeRun)
  {
    MergePartialResults();
    
    //Call a user hook: note this is before the next barrier
    //so threads execute this method asyncrhonouzly
    //(TerminateRun allows for synch via G4RunAction::EndOfRun)
    const G4UserWorkerInitialization* uwi
       = MyRunManager::GetMasterRunManager()->GetUserWorkerInitialization();
    if(uwi) uwi->WorkerRunEnd();
  }

  G4RunManager::RunTermination();
  //Signal this thread has finished envent-loop.
  //Note this will return only whan all threads reach this point
  MyRunManager::GetMasterRunManager()->ThisWorkerEndEventLoop();

}

void MyWorkerRunManager::TerminateEventLoop()
{
    if(verboseLevel>0 && !fakeRun)
    {
        timer->Stop();
        G4cout << "Thread-local run terminated." << G4endl;
        G4cout << "Run Summary" << G4endl;
        if(runAborted)
        { G4cout << "  Run Aborted after " << numberOfEventProcessed << " events processed." << G4endl; }
        else
        { G4cout << "  Number of events processed : " << numberOfEventProcessed << G4endl; }
        G4cout << "  "  << *timer << G4endl;
    }
}

/****************************
void MyWorkerRunManager::BeamOn(G4int n_event,const char* macroFile,G4int n_select)
{ 
  if(n_event>0) 
  { G4RunManager::BeamOn(n_event,macroFile,n_select); }
  else
  {
    // fake BeamOn.
    MyRunManager::GetMasterRunManager()->ThisWorkerReady();
    MyRunManager::GetMasterRunManager()->ThisWorkerEndEventLoop();
  }
}
******************************/

#include <G4AutoLock.hh>
namespace { G4Mutex ConstructScoringWorldsMutex = G4MUTEX_INITIALIZER; }
void MyWorkerRunManager::ConstructScoringWorlds()
{
    // Return if unnecessary
    G4ScoringManager* ScM = G4ScoringManager::GetScoringManagerIfExist();
    if(!ScM) return;
    G4int nPar = ScM->GetNumberOfMesh();
    if(nPar<1) return;

    // Update thread-local G4TransportationManager of all the world volumes
    kernel->WorkerUpdateWorldVolume();

    G4ScoringManager* masterScM = MyRunManager::GetMasterScoringManager();
    assert( masterScM != NULL );
    
    G4ParticleTable::G4PTblDicIterator* particleIterator
     = G4ParticleTable::GetParticleTable()->GetIterator();
    
    for(G4int iw=0;iw<nPar;iw++)
    {
      G4VScoringMesh* mesh = ScM->GetMesh(iw);
      if(fGeometryHasBeenDestroyed) mesh->GeometryHasBeenDestroyed();
      G4VPhysicalVolume* pWorld
       = G4TransportationManager::GetTransportationManager()
         ->IsWorldExisting(ScM->GetWorldName(iw));
      if(!pWorld)
      {
        G4ExceptionDescription ed;
        ed<<"Mesh name <"<<ScM->GetWorldName(iw)<<"> is not found in the master thread.";
        G4Exception("MyWorkerRunManager::ConstructScoringWorlds()","RUN79001",
                      FatalException,ed);
      }
      if(!(mesh->GetMeshElementLogical()))
      {
        G4AutoLock l(&ConstructScoringWorldsMutex);
        G4VScoringMesh* masterMesh = masterScM->GetMesh(iw);
        mesh->SetMeshElementLogical(masterMesh->GetMeshElementLogical());
        l.unlock();
        
        G4ParallelWorldProcess* theParallelWorldProcess = mesh->GetParallelWorldProcess();
        if(theParallelWorldProcess)
        { theParallelWorldProcess->SetParallelWorld(ScM->GetWorldName(iw)); }
        else
        {
          theParallelWorldProcess = new G4ParallelWorldProcess(ScM->GetWorldName(iw));
          mesh->SetParallelWorldProcess(theParallelWorldProcess);
          theParallelWorldProcess->SetParallelWorld(ScM->GetWorldName(iw));

          particleIterator->reset();
          while( (*particleIterator)() ){
            G4ParticleDefinition* particle = particleIterator->value();
            G4ProcessManager* pmanager = particle->GetProcessManager();
            if(pmanager)
            {
              pmanager->AddProcess(theParallelWorldProcess);
              if(theParallelWorldProcess->IsAtRestRequired(particle))
              { pmanager->SetProcessOrdering(theParallelWorldProcess, idxAtRest, 9900); }
              pmanager->SetProcessOrderingToSecond(theParallelWorldProcess, idxAlongStep);
              pmanager->SetProcessOrdering(theParallelWorldProcess, idxPostStep, 9900);
            } //if(pmanager)
          }//while
        }
      }
      mesh->WorkerConstruct(pWorld);
    }
}

void MyWorkerRunManager::SetUserInitialization(G4UserWorkerInitialization*)
{
    G4Exception("G4RunManager::SetUserInitialization(G4UserWorkerInitialization*)", "Run0118",
                FatalException, "This method should be used only with an instance of MyRunManager");
}

void MyWorkerRunManager::SetUserInitialization(G4UserWorkerThreadInitialization*)
{
    G4Exception("G4RunManager::SetUserInitialization(G4UserWorkerThreadInitialization*)", "Run0119",
                FatalException, "This method should be used only with an instance of MyRunManager");
}

void MyWorkerRunManager::SetUserInitialization(G4VUserActionInitialization*)
{
    G4Exception("G4RunManager::SetUserInitialization(G4VUserActionInitialization*)", "Run0120",
                FatalException, "This method should be used only with an instance of MyRunManager");
}

void MyWorkerRunManager::SetUserInitialization(G4VUserDetectorConstruction*)
{
    G4Exception("G4RunManager::SetUserInitialization(G4VUserDetectorConstruction*)", "Run0121",
                FatalException, "This method should be used only with an instance of MyRunManager");
}

void MyWorkerRunManager::SetUserInitialization(G4VUserPhysicsList* pl)
{
    pl->InitializeWorker();
    G4RunManager::SetUserInitialization(pl);
}

void MyWorkerRunManager::SetUserAction(G4UserRunAction* userAction)
{
    G4RunManager::SetUserAction(userAction);
    if(userAction) userAction->SetMaster(false);
}

void MyWorkerRunManager::SetupDefaultRNGEngine()
{
    const CLHEP::HepRandomEngine* mrnge = MyRunManager::GetMasterRunManager()->getMasterRandomEngine();
    assert(mrnge);//Master has created RNG
    const G4UserWorkerThreadInitialization* uwti
      =MyRunManager::GetMasterRunManager()->GetUserWorkerThreadInitialization();
    uwti->SetupRNGEngine(mrnge);
}


//Forward calls (avoid GCC compilation warnings)
void MyWorkerRunManager::SetUserAction(G4UserEventAction* ua)
{
    G4RunManager::SetUserAction(ua);
}

void MyWorkerRunManager::SetUserAction(G4VUserPrimaryGeneratorAction* ua)
{
    G4RunManager::SetUserAction(ua);
}

void MyWorkerRunManager::SetUserAction(G4UserStackingAction* ua)
{
    G4RunManager::SetUserAction(ua);
}

void MyWorkerRunManager::SetUserAction(G4UserTrackingAction* ua)
{
    G4RunManager::SetUserAction(ua);
}

void MyWorkerRunManager::SetUserAction(G4UserSteppingAction* ua)
{
    G4RunManager::SetUserAction(ua);
}

void MyWorkerRunManager::StoreRNGStatus(const G4String& fn )
{
    std::ostringstream os;
    os << randomNumberStatusDir << "G4Worker"<<workerContext->GetThreadId()<<"_"<<fn <<".rndm";
    G4Random::saveEngineStatus(os.str().c_str());
}

void MyWorkerRunManager::DoWork()
{
  //   TIMEMORY_AUTO_TIMER("");
  MyRunManager* mrm = MyRunManager::GetMasterRunManager();
  // MyRunManager::WorkerActionRequest nextAction = mrm->ThisWorkerWaitForNextAction();
  // while( nextAction != MyRunManager::WorkerActionRequest::ENDWORKER )
  // {
  //   if( nextAction == MyRunManager::WorkerActionRequest::NEXTITERATION ) // start the next run
  //   {
      //The following code deals with changing materials between runs
      static thread_local G4bool skipInitialization = true;
      if(skipInitialization)
      {
        // re-initialization is not necessary for the first run
        skipInitialization = false;
      }
      else
      {
//        ReinitializeGeometry();
          workerContext->UpdateGeometryAndPhysicsVectorFromMaster();
      }

      // Execute UI commands stored in the master UI manager
      std::vector<G4String> cmds = mrm->GetCommandStack();
      G4UImanager* uimgr = G4UImanager::GetUIpointer(); //TLS instance
      std::vector<G4String>::const_iterator it = cmds.begin();
      for(;it!=cmds.end();it++)
      { uimgr->ApplyCommand(*it); }
      //Start this run
      G4int numevents = mrm->GetNumberOfEventsToBeProcessed();
      G4String macroFile = mrm->GetSelectMacro();
      G4int numSelect = mrm->GetNumberOfSelectEvents();
      if ( macroFile == "" || macroFile == " " )
      {
          this->BeamOn(numevents);
      }
      else
      {
          this->BeamOn(numevents,macroFile,numSelect);
      }
  //   }
  //   else if (nextAction == MyRunManager::WorkerActionRequest::PROCESSUI ) {
  //       std::vector<G4String> cmds = mrm->GetCommandStack();
  //       G4UImanager* uimgr = G4UImanager::GetUIpointer(); //TLS instance
  //       std::vector<G4String>::const_iterator it = cmds.begin();
  //       for(;it!=cmds.end();it++)
  //       { uimgr->ApplyCommand(*it); }
  //       mrm->ThisWorkerProcessCommandsStackDone();
  //   }
  //   else
  //   {
  //     G4ExceptionDescription d;
  //     d<<"Cannot continue, this worker has been requested an unknown action: "
  //      <<static_cast<std::underlying_type<MyRunManager::WorkerActionRequest>::type>(nextAction);
  //     G4Exception("MyWorkerRunManager::DoWork","Run0104",FatalException,d);
  //   }

  //   //Now wait for master thread to signal new action to be performed
  //   nextAction = mrm->ThisWorkerWaitForNextAction();
  // } //No more actions to perform

  return;
}
