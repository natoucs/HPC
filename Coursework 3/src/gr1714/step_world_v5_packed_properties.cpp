#include "heat.hpp"

#include <stdexcept>
#include <cmath>
#include <cstdint>
#include <memory>
#include <cstdio>
#include <string>
#include <fstream>
#include <streambuf>

#define __CL_ENABLE_EXCEPTIONS 
#include "CL/cl.hpp"

namespace hpce{

namespace gr1714{

std::string LoadSource(const char *fileName)
{
	// TODO : Don't forget to change your_login here
	std::string baseDir="src/gr1714";
	if(getenv("HPCE_CL_SRC_DIR")){
		baseDir=getenv("HPCE_CL_SRC_DIR");
	}
	
	std::string fullName=baseDir+"/"+fileName;
	
	// Open a read-only binary stream over the file
	std::ifstream src(fullName, std::ios::in | std::ios::binary);
	if(!src.is_open())
		throw std::runtime_error("LoadSource : Couldn't load cl file from '"+fullName+"'.");
	
	// Read all characters of the file into a string
	return std::string(
		(std::istreambuf_iterator<char>(src)), // Node the extra brackets.
        std::istreambuf_iterator<char>()
	);
}
	
void StepWorldV5PackedProperties(world_t &world, float dt, unsigned n)
{
	std::vector<cl::Platform> platforms;
	
	cl::Platform::get(&platforms);
	if(platforms.size()==0)
		throw std::runtime_error("No OpenCL platforms found.");

	std::cerr<<"Found "<<platforms.size()<<" platforms\n";
	for(unsigned i=0;i<platforms.size();i++){
		std::string vendor=platforms[i].getInfo<CL_PLATFORM_VENDOR>();
		std::cerr<<"  Platform "<<i<<" : "<<vendor<<"\n";
	}

	int selectedPlatform=0;
	if(getenv("HPCE_SELECT_PLATFORM")){
		selectedPlatform=atoi(getenv("HPCE_SELECT_PLATFORM"));
	}
	std::cerr<<"Choosing platform "<<selectedPlatform<<"\n";
	cl::Platform platform=platforms.at(selectedPlatform);  

	std::vector<cl::Device> devices;
	platform.getDevices(CL_DEVICE_TYPE_ALL, &devices);	
	if(devices.size()==0){
		throw std::runtime_error("No opencl devices found.\n");
	}

	std::cerr<<"Found "<<devices.size()<<" devices\n";
	for(unsigned i=0;i<devices.size();i++){
		std::string name=devices[i].getInfo<CL_DEVICE_NAME>();
		std::cerr<<"  Device "<<i<<" : "<<name<<"\n";
	}

	int selectedDevice=0;
	if(getenv("HPCE_SELECT_DEVICE")){
		selectedDevice=atoi(getenv("HPCE_SELECT_DEVICE"));
	}
	std::cerr<<"Choosing device "<<selectedDevice<<"\n";
	cl::Device device=devices.at(selectedDevice);

	cl::Context context(devices);

	std::string kernelSource=hpce::gr1714::LoadSource("step_world_v5_packed_properties.cl");
	
	cl::Program::Sources sources;	// A vector of (data,length) pairs
	sources.push_back(std::make_pair(kernelSource.c_str(), kernelSource.size()+1));	// push on our single string
	
	cl::Program program(context, sources);
	try{
		program.build(devices);
	}catch(...){
		for(unsigned i=0;i<devices.size();i++){
			std::cerr<<"Log for device "<<devices[i].getInfo<CL_DEVICE_NAME>()<<":\n\n";
			std::cerr<<program.getBuildInfo<CL_PROGRAM_BUILD_LOG>(devices[i])<<"\n\n";
		}
		throw;
	}

	unsigned w=world.w, h=world.h;
	
	float outer=world.alpha*dt;		// We spread alpha to other cells per time
	float inner=1-outer/4;				// Anything that doesn't spread stays


	size_t cbBuffer=4*world.w*world.h;
	cl::Buffer buffProperties(context, CL_MEM_READ_ONLY, cbBuffer);
	cl::Buffer buffState(context, CL_MEM_READ_WRITE, cbBuffer);
	cl::Buffer buffBuffer(context, CL_MEM_READ_WRITE, cbBuffer);

	cl::Kernel kernel(program, "kernel_xy");
	kernel.setArg(0, buffState);
	kernel.setArg(1, buffProperties);
	kernel.setArg(2, buffBuffer);
	kernel.setArg(3, inner);
	kernel.setArg(4, outer);

	cl::CommandQueue queue(context, device);

	std::vector<uint32_t> packed(w*h, 0);
	// representation is 00,00,00,00,00
	// corresponds to right, left, below, above, inner

	for(unsigned index = 0; index < w * h; index++){
		packed[index] = world.properties[index];
		if(!(
			(world.properties[index] & Cell_Fixed) || 
			(world.properties[index] & Cell_Insulator)
		)){
			// Cell above
			if(world.properties[index-w] & Cell_Insulator) {
				packed[index] += 0x8;
			}
			
			// Cell below
			if(world.properties[index+w] & Cell_Insulator) {
				packed[index] += 0x20;
			}
			
			// Cell left
			if(world.properties[index-1] & Cell_Insulator) {
				packed[index] += 0x80;
			}
			
			// Cell right
			if(world.properties[index+1] & Cell_Insulator) {
				packed[index] += 0x200;
			}
		}
	}

	queue.enqueueWriteBuffer(buffProperties, CL_TRUE, 0, cbBuffer, &packed[0]);
	queue.enqueueWriteBuffer(buffState, CL_TRUE, 0, cbBuffer, &world.state[0]);

	cl::NDRange offset(0, 0);				// Always start iterations at x=0, y=0
	cl::NDRange globalSize(w, h);	// Global size must match the original loops
	cl::NDRange localSize=cl::NullRange;	// We don't care about local size
	
	for(unsigned t=0;t<n;t++){
		kernel.setArg(0, buffState);
		kernel.setArg(2, buffBuffer);

		queue.enqueueNDRangeKernel(kernel, offset, globalSize, localSize);

		queue.enqueueBarrier();

		std::swap(buffState, buffBuffer);
	
		world.t += dt; // We have moved the world forwards in time
		
	} // end of for(t...
	queue.enqueueReadBuffer(buffState, CL_TRUE, 0, cbBuffer, &world.state[0]);
}

}; //namespace gr1714
}; // namepspace hpce

int main(int argc, char *argv[])
{

	float dt=0.1;
	unsigned n=1;
	bool binary=false;
	
	if(argc>1){
		dt=(float)strtod(argv[1], NULL);
	}
	if(argc>2){
		n=atoi(argv[2]);
	}
	if(argc>3){
		if(atoi(argv[3]))
			binary=true;
	}
	
	try{
		hpce::world_t world=hpce::LoadWorld(std::cin);
		std::cerr<<"Loaded world with w="<<world.w<<", h="<<world.h<<std::endl;
		
		std::cerr<<"Stepping by dt="<<dt<<" for n="<<n<<std::endl;
		hpce::gr1714::StepWorldV5PackedProperties(world, dt, n);
		
		hpce::SaveWorld(std::cout, world, binary);
	}catch(const std::exception &e){
		std::cerr<<"Exception : "<<e.what()<<std::endl;
		return 1;
	}
		
	return 0;
}
