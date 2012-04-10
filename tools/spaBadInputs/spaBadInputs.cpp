#include <assert.h>
#include <string.h>
#include <iostream>
#include <fstream>
#include <string>
#include <utility>
#include <map>

#include "llvm/Support/CommandLine.h"

// FIXME: Ugh, this is gross. But otherwise our config.h conflicts with LLVMs.
#undef PACKAGE_BUGREPORT
#undef PACKAGE_NAME
#undef PACKAGE_STRING
#undef PACKAGE_TARNAME
#undef PACKAGE_VERSION

#include <llvm/Support/MemoryBuffer.h>
#include <klee/Constraints.h>
#include <klee/ExprBuilder.h>
#include <klee/Solver.h>
#include <klee/Init.h>
#include <klee/Expr.h>
#include <klee/ExprBuilder.h>
#include <klee/Solver.h>
#include <../../lib/Core/Memory.h>

#include "spa/SPA.h"
#include "spa/PathLoader.h"


namespace {
	llvm::cl::opt<std::string> ClientPathFile("client", llvm::cl::desc(
		"Specifies the client path file."));

	llvm::cl::opt<std::string> ServerPathFile("server", llvm::cl::desc(
		"Specifies the server path file."));
}

class InvalidPathFilter : public SPA::PathFilter {
public:
	bool checkPath( SPA::Path &path ) {
		return path.getTag( SPA_VALIDPATH_TAG ) != SPA_VALIDPATH_VALUE;
	}
};

void showConstraints( klee::ConstraintManager &cm ) {
	for ( klee::ConstraintManager::const_iterator it = cm.begin(), ie = cm.end(); it != ie; it++ )
		std::cerr << *it << std::endl;
}

int main(int argc, char **argv, char **envp) {
	// Fill up every global cl::opt object declared in the program
	cl::ParseCommandLineOptions( argc, argv, "Systematic Protocol Analyzer - Bad Input Generator" );

	assert( ClientPathFile.size() > 0 && "No client path file specified." );
	std::cerr << "Loading client path data." << std::endl;
	std::ifstream pathFile( ClientPathFile.getValue().c_str() );
	assert( pathFile.is_open() && "Unable to open path file." );
	SPA::PathLoader clientPathLoader( pathFile );
	std::set<SPA::Path *> clientPaths;
	while ( SPA::Path *path = clientPathLoader.getPath() )
		clientPaths.insert( path );
	pathFile.close();
	std::cerr << "Found " << clientPaths.size() << " client paths." << std::endl;
	
	assert( ServerPathFile.size() > 0 && "No server path file specified." );
	std::cerr << "Loading server path data." << std::endl;
	pathFile.open( ServerPathFile.getValue().c_str() );
	assert( pathFile.is_open() && "Unable to open path file." );
	SPA::PathLoader serverPathLoader( pathFile );
	serverPathLoader.setFilter( new InvalidPathFilter() );
	std::set<SPA::Path *> serverPaths;
	while ( SPA::Path *path = serverPathLoader.getPath() )
		serverPaths.insert( path );
	pathFile.close();
	std::cerr << "Found " << serverPaths.size() << " server paths marked as invalid." << std::endl;
	
	klee::ExprBuilder *exprBuilder = klee::createDefaultExprBuilder();
	klee::Solver *solver = new klee::STPSolver( false, true );
	solver = klee::createCexCachingSolver(solver);
	solver = klee::createCachingSolver(solver);
	solver = klee::createIndependentSolver(solver);

	unsigned long numClientPaths = 0;
	for ( std::set<SPA::Path *>::iterator cit = clientPaths.begin(), cie = clientPaths.end(); cit != cie; cit++, numClientPaths++ ) {
		unsigned long numServerPaths = 0;
		for ( std::set<SPA::Path *>::iterator sit = serverPaths.begin(), sie = serverPaths.end(); sit != sie; sit++, numServerPaths++ ) {
			std::cerr << "Processing client path " << (numClientPaths + 1) << "/" << clientPaths.size()
				<< " with server path " << (numServerPaths + 1) << "/" << serverPaths.size() << "." << std::endl;
			klee::ConstraintManager cm;
			// Add client path constraints.
			for ( klee::ConstraintManager::const_iterator it = (*cit)->getConstraints().begin(), ie = (*cit)->getConstraints().end(); it != ie; it++ )
				cm.addConstraint( *it );
			// Add server path constraints.
			for ( klee::ConstraintManager::const_iterator it = (*sit)->getConstraints().begin(), ie = (*sit)->getConstraints().end(); it != ie; it++ )
				cm.addConstraint( *it );
			// Add client output values = server input array constraint.
			for ( std::map<std::string, const klee::Array *>::const_iterator it = (*sit)->beginSymbols(), ie = (*sit)->endSymbols(); it != ie; it++ ) {
				if ( it->first.compare( 0, strlen( SPA_MESSAGE_INPUT_PREFIX ), SPA_MESSAGE_INPUT_PREFIX ) == 0 ) {
					std::string type = it->first.substr( strlen( SPA_MESSAGE_INPUT_PREFIX ), std::string::npos );
					std::cerr << "	Connecting message type: " << type << std::endl;
					const klee::Array *serverIn = it->second;
					std::string clientOutName = std::string( SPA_MESSAGE_OUTPUT_PREFIX ) + type;
					assert( (*cit)->getSymbol( clientOutName ) && "Server consumes a message type not generated by client." );
					assert( serverIn->size == (*cit)->getSymbolValueSize( clientOutName ) && "Client and server message size mismatch." );

					for ( size_t offset = 0; offset < serverIn->size; offset++ ) {
						klee::UpdateList ul( serverIn, 0 );
						cm.addConstraint( exprBuilder->Eq(
							exprBuilder->Read( ul, exprBuilder->Constant( offset, klee::Expr::Int32 ) ),
							(*cit)->getSymbolValue( clientOutName, offset ) ) );
					}
				}
			}
// 			std::cerr << "Path pair constraints:" << std::endl;
// 			showConstraints( cm );

			// Solve for client API inputs.
			std::vector<std::string> objectNames;
			std::vector<const klee::Array*> objects;
			for ( std::map<std::string, const klee::Array *>::const_iterator it = (*cit)->beginSymbols(), ie = (*cit)->endSymbols(); it != ie; it++ ) {
				if ( it->first.compare( 0, strlen( SPA_API_INPUT_PREFIX ), SPA_API_INPUT_PREFIX ) == 0 ) {
					objectNames.push_back( it->first.substr( strlen( SPA_API_INPUT_PREFIX ) ) );
					objects.push_back( it->second );
				}
			}

// 			std::cerr << "Solving constraint for:" << std::endl;
// 			for ( size_t i = 0; i < objects.size(); i++ )
// 				std::cerr << "	" << objectNames[i] << ":" << objects[i] << std::endl;
			std::vector< std::vector<unsigned char> > result;
			if ( solver->getInitialValues( klee::Query( cm, exprBuilder->False() ), objects, result ) ) {
				std::cerr << "Found solution." << std::endl;
				for ( size_t i = 0; i < result.size(); i++ ) {
					std::cout << objectNames[i] << "[" << result[i].size() << "] = {";
					for ( std::vector<unsigned char>::iterator it = result[i].begin(), ie = result[i].end(); it != ie; it++ )
						std::cout << " " << (int) *it << (it != result[i].end() ? "," : "");
					std::cout << " };" << std::endl;
				}
				std::cout << "// -----------------------------------------------------------------------------" << std::endl;
			} else {
				std::cerr << "Could not solve constraint." << std::endl;
			}
		}
	}
}
