#include "HitResiduals.h"

#include <iostream>
#include <algorithm>    // std::sort

#include <EVENT/LCCollection.h>
#include <EVENT/LCObject.h>
#include <EVENT/TrackerHit.h>
#include <EVENT/TrackerHitPlane.h>
#include "MarlinTrk/MarlinTrkUtils.h"
#include "MarlinTrk/IMarlinTrack.h"

#include <UTIL/BitField64.h>
#include <UTIL/ILDConf.h>

#include "DD4hep/LCDD.h"
#include "DD4hep/DD4hepUnits.h"


// ----- include for verbosity dependend logging ---------
#include "marlin/VerbosityLevels.h"

#include <marlin/AIDAProcessor.h>

#include "fpcompare.h"

#include "TFile.h"
#include "TTree.h"
#include "TVector3.h"
#include "TLorentzVector.h"
#include <cmath>

using namespace lcio ;
using namespace marlin ;
// namespace MarlinTrk{
//   class IMarlinTrkSystem ;
// }
// using namespace MarlinTrk ;


struct RSort {  // sort hits wtr to r - smallest r fisrt
  inline bool operator()( const lcio::LCObject* l, const lcio::LCObject* r) {
    double R1 = sqrt( pow(((const lcio::TrackerHitPlane*) l)->getPosition()[0],2) + pow(((const lcio::TrackerHitPlane*) l)->getPosition()[1],2));
    double R2 = sqrt( pow(((const lcio::TrackerHitPlane*) r)->getPosition()[0],2) + pow(((const lcio::TrackerHitPlane*) r)->getPosition()[1],2));
    return CxxUtils::fpcompare::less( R1, R2 );
  }
};

struct ZSort {  // sort hits wtr to z - smallest first
  inline bool operator()( const lcio::LCObject* l, const lcio::LCObject* r) {      
    double Z1 = fabs( ((const lcio::TrackerHitPlane*) l)->getPosition()[2] );
    double Z2 = fabs( ((const lcio::TrackerHitPlane*) r)->getPosition()[2] );
    return CxxUtils::fpcompare::less( Z1, Z2 );
  }
};

 

HitResiduals aHitResiduals ;


HitResiduals::HitResiduals() : Processor("HitResiduals") {

    // modify processor description
    _description = "HitResiduals plots te residual between the track fit and the hit in the local coordinate system u,v,w." ;


    // register steering parameters: name, description, class-variable, default value
    registerInputCollection( LCIO::TRACK,
			      "TrackCollectionName" , 
			      "Name of the input track collection"  ,
			      _inputTrkColName ,
			      std::string("RefittedTracks") ) ;
	
    registerProcessorParameter( "outFileName",
				"Name of the output root file",
				_outFileName,
				std::string("residuals.root")
				);

    registerProcessorParameter( "treeName",
				"Name of the tree",
				_treeName,
				std::string("tree")
				);



    registerProcessorParameter("MultipleScatteringOn",
			       "Use MultipleScattering in Fit",
			       _MSOn,
			       bool(true));
  
    registerProcessorParameter("EnergyLossOn",
			       "Use Energy Loss in Fit",
			       _ElossOn,
			       bool(true));
  
    registerProcessorParameter("SmoothOn",
			       "Smooth All Mesurement Sites in Fit",
			       _SmoothOn,
			       bool(false));
  
    registerProcessorParameter("MaxChi2Increment",
			       "Maximum increment allowed for the chi2",
			       _Max_Chi2_Incr,
			       double(1000.));

}



void HitResiduals::init() { 

  streamlog_out(DEBUG) << "   init called  " << std::endl ;

  // usually a good idea to
  printParameters() ;

  _nRun = 0 ;
  _nEvt = 0 ;


  //tree

  _out = new TFile(_outFileName.c_str(),"RECREATE");
  _tree = new TTree(_treeName.c_str(),_treeName.c_str());
  int bufsize = 32000; //default buffer size 32KB

  _tree->Branch("nRun",&_nRun,"nRun/I");
  _tree->Branch("nEvt",&_nEvt,"nEvt/I");
  _tree->Branch("resU", "std::vector<double >",&_resU,bufsize,0);
  _tree->Branch("resV", "std::vector<double >",&_resV,bufsize,0);
  _tree->Branch("subdet", "std::vector<int >",&_subdet,bufsize,0);
  _tree->Branch("layer", "std::vector<int >",&_layer,bufsize,0);
 


  //lcdd and bfield

  DD4hep::Geometry::LCDD& lcdd = DD4hep::Geometry::LCDD::getInstance();

  const double pos[3]={0,0,0}; 
  double bFieldVec[3]={0,0,0}; 
  lcdd.field().magneticField(pos,bFieldVec); // get the magnetic field vector from DD4hep
  _bField = bFieldVec[2]/dd4hep::tesla; // z component at (0,0,0)


  //trksystem for marlin track

  _trksystem =  MarlinTrk::Factory::createMarlinTrkSystem( "DDKalTest" , marlin::Global::GEAR , "" ) ;
  
  if( _trksystem == 0 ) throw EVENT::Exception( std::string("  Cannot initialize MarlinTrkSystem of Type: ") + std::string("DDKalTest") );
  
  _trksystem->setOption( IMarlinTrkSystem::CFG::useQMS,        _MSOn ) ;
  _trksystem->setOption( IMarlinTrkSystem::CFG::usedEdx,       _ElossOn) ;
  _trksystem->setOption( IMarlinTrkSystem::CFG::useSmoothing,  _SmoothOn) ;
  _trksystem->init() ;  
  

  //surface map 

  SurfaceManager& surfMan = *lcdd.extension< SurfaceManager >() ;
  // const SurfaceMap& _surfMap = *surfMan.map( "world" ) ;
  _surfMap = *surfMan.map( "world" ) ;

  
}


void HitResiduals::processRunHeader( LCRunHeader* run) { 

    _nRun++ ;
} 



void HitResiduals::processEvent( LCEvent * evt ) { 


  // this gets called for every event 
  // usually the working horse ...


  streamlog_out(DEBUG2) << "----- _nEvt = " << _nEvt << std::endl;

  //clear vectors
  _resU.clear();
  _resV.clear();
  _subdet.clear();
  _layer.clear();



  UTIL::BitField64 cellid_decoder( lcio::ILDCellID0::encoder_string ) ; 	    
  UTIL::BitField64 encoder( lcio::ILDCellID0::encoder_string ) ; 	    
  encoder.reset() ;  // reset to 0
  int layerID = encoder.lowWord() ;  
  int elementID = 0 ;    

  LCCollection* inputTrkCol = this->GetCollection( evt, _inputTrkColName );
  if (inputTrkCol!=0) {

   // std::string cellIDEcoding = inputTrkCol->getParameters().getStringVal("CellIDEncoding") ;  
   // UTIL::BitField64 cellid_decoder( cellIDEcoding ) ;

    for( int i= 0; i < inputTrkCol->getNumberOfElements(); i++ ) {

      Track* track = dynamic_cast<Track*>( inputTrkCol->getElementAt(i) );
      MarlinTrk::IMarlinTrack* marlin_trk = _trksystem->createTrack();

      EVENT::TrackerHitVec trkHits = track->getTrackerHits() ;
      // sort(trkHits.begin(), trkHits.end(), RSort() );
      // reverse(trkHits.begin(), trkHits.end());
	
      for( EVENT::TrackerHitVec::iterator it = trkHits.begin() ; it != trkHits.end() ; ++it ){
      	marlin_trk->addHit(*it);	
      }//end loop on hits
      //int init_status = FitInit2(track, marlin_trk);          
      int init_status = FitInitFromLCIOTrackState(track, marlin_trk);          
      //int fit_status = marlin_trk->fit(); 
      //streamlog_out(DEBUG2) << "fit status (good = 0) = " << fit_status << std::endl;


      for( EVENT::TrackerHitVec::iterator it = trkHits.begin(); it != trkHits.end(); ++it ){

	DD4hep::long64 id = (*it)->getCellID0() ;
	cellid_decoder.setValue( id ) ;
	streamlog_out(DEBUG1) << "id = " << id << std::endl;

	int layer = cellid_decoder["layer"].value();
	int subdet = cellid_decoder["subdet"].value();
	streamlog_out(DEBUG1) << "layer = " << layer << std::endl;
	streamlog_out(DEBUG1) << "subdet = " << subdet << std::endl;

	encoder[lcio::ILDCellID0::subdet] = subdet;
	encoder[lcio::ILDCellID0::layer]  = layer;   
	layerID = encoder.lowWord();  
	streamlog_out(DEBUG1) << "layerID = " << layerID << std::endl;

	TrackStateImpl trkState;
	double chi2 = 0 ;
	int ndf = 0 ;
	if ( marlin_trk->propagateToLayer( layerID, trkState, chi2, ndf, elementID, IMarlinTrack::modeClosest) == MarlinTrk::IMarlinTrack::success) {
	  const float* pivot = trkState.getReferencePoint();
	  double fitX  = pivot[0];
	  double fitY  = pivot[1];
	  streamlog_out(DEBUG2) << "----- fit x, y, r = " << fitX <<"   "<< fitY <<"   "<< sqrt(pow(fitX,2)+pow(fitY,2)) <<std::endl;
	  const double* hit_pos = (*it)->getPosition();
	  double hitX = hit_pos[0];
	  double hitY = hit_pos[1];
	  streamlog_out(DEBUG2) << "----- hit x, y, r = " << hitX <<"   "<< hitY <<"   "<< sqrt(pow(hitX,2)+pow(hitY,2)) <<std::endl;

	  SurfaceMap::const_iterator si = _surfMap.find(id);
	  ISurface* surf = (si != _surfMap.end() ?  si->second  : 0);

	  DDSurfaces::Vector3D fit_global(pivot[0],pivot[1],pivot[2]);                                   
	  DDSurfaces::Vector2D fit_local = surf->globalToLocal( dd4hep::mm * fit_global );  
	
	  double fitU = fit_local[0];
	  double fitV = fit_local[1];

	  DDSurfaces::Vector3D hit_global(hit_pos[0],hit_pos[1],hit_pos[2]);
	  DDSurfaces::Vector2D hit_local = surf->globalToLocal( dd4hep::mm * hit_global );  
	
	  double hitU = hit_local[0];
	  double hitV = hit_local[1];	

	  double resU = fitU-hitU;
	  double resV = fitV-hitV;
	  streamlog_out(DEBUG4) << "----- resU = " << resU << std::endl;
	  streamlog_out(DEBUG4) << "----- resV = " << resV << std::endl;

	  _resU.push_back(resU);
	  _resV.push_back(resV);
	  _subdet.push_back(subdet);
	  _layer.push_back(layer);

 	} else streamlog_out(DEBUG4) << "FAIL" << std::endl;
     
	
	// IT  WORKS TOO A BIT DIFFERENT PIVOT r BUT OKISH
	// TrackStateImpl *trkState = new TrackStateImpl();
	// double chi2 = 0.;
	// int ndf = 0;
	// double chi2_increment = -1;
	// int errcode = marlin_trk->getTrackState(*it, *trkState, chi2, ndf);
	// streamlog_out(MESSAGE) << "----- errcode = " << errcode << std::endl;
	// streamlog_out(MESSAGE) << "----- trkState, chi2, ndf = " << trkState << "   "<< chi2 << "   "<< ndf<< std::endl;
	// const float* pivot = trkState->getReferencePoint();
	// double fitX  = pivot[0];
	// double fitY  = pivot[1];
	// streamlog_out(MESSAGE) << "----- fitX = " << fitX << std::endl;
	// streamlog_out(MESSAGE) << "----- fitY = " << fitY << std::endl;
	// streamlog_out(MESSAGE) << "----- pivot r = " << sqrt(pow(fitX,2)+pow(fitY,2)) << std::endl;

	

      }//end loop on hits


    }//end loop on tracks
  }

  


  /////////////////////////////////////
  /////////////////////////////////////


  _tree->Fill();


  streamlog_out(DEBUG) << "   processing event: " << evt->getEventNumber() 
		       << "   in run:  " << evt->getRunNumber() << std::endl ;



  _nEvt ++ ;

}



void HitResiduals::check( LCEvent * evt ) { 
    // nothing to check here - could be used to fill checkplots in reconstruction processor
}


void HitResiduals::end(){ 

  _tree->Write();
  _out->Write();
  _out->Close();

  std::cout << "HitResiduals::end()  " << name() 
    	    << " processed " << _nEvt << " events in " << _nRun << " runs "
    	    << std::endl ;

}






LCCollection* HitResiduals::GetCollection( LCEvent*& evt, std::string colName ){
  
  LCCollection* col = NULL;
    
  try{
    col = evt->getCollection( colName.c_str() ) ;
    streamlog_out( DEBUG3 ) << " --> " << colName.c_str() << " track collection found in event = " << col << " number of elements " << col->getNumberOfElements() << std::endl;
  }
  catch(DataNotAvailableException &e){
    streamlog_out( DEBUG3 ) << " --> " << colName.c_str() <<  " collection absent in event" << std::endl;     
  }
  
  return col; 
  
}





int HitResiduals::FitInit2( Track*& track, MarlinTrk::IMarlinTrack*& _marlinTrk ){
  
  
  TrackStateImpl trackState( TrackState::AtOther, 
			     track->getD0(), 
			     track->getPhi(), 
			     track->getOmega(), 
			     track->getZ0(), 
			     track->getTanLambda(), 
			     track->getCovMatrix(), 
			     track->getReferencePoint()
			     );
  
  _marlinTrk->initialise( trackState, _bField, IMarlinTrack::forward ) ;
  
  return IMarlinTrack::success ;   
  

}





int HitResiduals::FitInitFromLCIOTrackState( Track*& track, MarlinTrk::IMarlinTrack*& _marlinTrk ){
  
  
  TrackStateImpl trackState( *(track->getTrackState(TrackState::AtFirstHit)) );  

  _marlinTrk->initialise( trackState, _bField, IMarlinTrack::forward ) ;

  // TrackStateImpl trackState( *(track->getTrackState(TrackState::AtLastHit)) );

  // _marlinTrk->initialise( trackState, _bField, IMarlinTrack::backward ) ;
  
  return IMarlinTrack::success ;   
  

}
