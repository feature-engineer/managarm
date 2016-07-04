
#include <frigg/types.hpp>
#include <frigg/traits.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/debug.hpp>
#include <frigg/initializer.hpp>
#include <frigg/libc.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
#include <frigg/smart_ptr.hpp>
#include <frigg/string.hpp>
#include <frigg/vector.hpp>
#include <frigg/optional.hpp>
#include <frigg/tuple.hpp>
#include <frigg/hashmap.hpp>
#include <frigg/protobuf.hpp>
#include <frigg/chain-all.hpp>

#include <frigg/glue-hel.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include "common.hpp"
#include "device.hpp"
#include "vfs.hpp"
#include "process.hpp"
#include "exec.hpp"
#include "dev_fs.hpp"
#include "pts_fs.hpp"
#include "sysfile_fs.hpp"
#include "extern_fs.hpp"

#include "posix.frigg_pb.hpp"
#include "mbus.frigg_pb.hpp"

bool traceRequests = false;

helx::EventHub eventHub = helx::EventHub::create();
helx::Client mbusConnect;
helx::Pipe ldServerPipe;
helx::Pipe mbusPipe;

// TODO: this could be handled better
helx::Pipe initrdPipe;

// TODO: this is a ugly hack
MountSpace *initMountSpace;

HelHandle ringBuffer;
HelRingBuffer *ringItem;

// --------------------------------------------------------
// RequestClosure
// --------------------------------------------------------

struct RequestClosure : frigg::BaseClosure<RequestClosure> {
	RequestClosure(StdSharedPtr<helx::Pipe> pipe, StdSharedPtr<Process> process, int iteration)
	: pipe(frigg::move(pipe)), process(process), iteration(iteration) { }
	
	void operator() ();
	
private:
	void recvRequest(HelError error, int64_t msg_request, int64_t msg_seq,
			size_t index, size_t offset, size_t length);

	void processRequest(managarm::posix::ClientRequest<Allocator> request, int64_t msg_request);
	
	StdSharedPtr<helx::Pipe> pipe;
	StdSharedPtr<Process> process;
	int iteration;
};

void RequestClosure::processRequest(managarm::posix::ClientRequest<Allocator> request,
		int64_t msg_request) {
	// check the iteration number to prevent this process from being hijacked
	if(process && iteration != process->iteration) {
		auto action = frigg::compose([=] (auto serialized) {
			managarm::posix::ServerResponse<Allocator> response(*allocator);
			response.set_error(managarm::posix::Errors::DEAD_FORK);
			response.SerializeToString(serialized);
			
			return pipe->sendStringResp(serialized->data(), serialized->size(),
					eventHub, msg_request, 0)
			+ frigg::apply([=] (HelError error) { HEL_CHECK(error); });
		}, frigg::String<Allocator>(*allocator));
		
		frigg::run(frigg::move(action), allocator.get());
		return;
	}

	if(request.request_type() == managarm::posix::ClientRequestType::INIT) {
		assert(!process);

		auto action = frigg::compose([=] (auto serialized) {
			process = Process::init();
			initMountSpace = process->mountSpace;

			auto device = frigg::makeShared<KernelOutDevice>(*allocator);

			unsigned int major, minor;
			DeviceAllocator &char_devices = process->mountSpace->charDevices;
			char_devices.allocateDevice("misc",
					frigg::staticPtrCast<Device>(frigg::move(device)), major, minor);
		
			auto initrd_fs = frigg::construct<extern_fs::MountPoint>(*allocator,
					frigg::move(initrdPipe));
			auto initrd_path = frigg::String<Allocator>(*allocator, "/initrd");
			initMountSpace->allMounts.insert(initrd_path, initrd_fs);

			auto dev_fs = frigg::construct<dev_fs::MountPoint>(*allocator);
			auto inode = frigg::makeShared<dev_fs::CharDeviceNode>(*allocator, major, minor);
			dev_fs->getRootDirectory()->entries.insert(frigg::String<Allocator>(*allocator, "helout"),
					frigg::staticPtrCast<dev_fs::Inode>(frigg::move(inode)));
			auto dev_root = frigg::String<Allocator>(*allocator, "/dev");
			process->mountSpace->allMounts.insert(dev_root, dev_fs);

			auto pts_fs = frigg::construct<pts_fs::MountPoint>(*allocator);
			auto pts_root = frigg::String<Allocator>(*allocator, "/dev/pts");
			process->mountSpace->allMounts.insert(pts_root, pts_fs);
			
			auto sysfile_fs = frigg::construct<sysfile_fs::MountPoint>(*allocator);
			auto sysfile_root = frigg::String<Allocator>(*allocator, "/dev/sysfile");
			process->mountSpace->allMounts.insert(sysfile_root, sysfile_fs);

			managarm::posix::ServerResponse<Allocator> response(*allocator);
			response.set_error(managarm::posix::Errors::SUCCESS);
			response.SerializeToString(serialized);
			
			return pipe->sendStringResp(serialized->data(), serialized->size(),
					eventHub, msg_request, 0)
			+ frigg::apply([=] (HelError error) {
				HEL_CHECK(error);
			});
		}, frigg::String<Allocator>(*allocator));
		
		frigg::run(frigg::move(action), allocator.get());
	}else if(request.request_type() == managarm::posix::ClientRequestType::GET_PID) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] GET_PID" << frigg::EndLog();

			auto action = frigg::compose([=] (auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::SUCCESS);
				response.set_pid(process->pid);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::apply([=] (HelError error) { HEL_CHECK(error); });
			}, frigg::String<Allocator>(*allocator));

			frigg::run(action, allocator.get());
	}else if(request.request_type() == managarm::posix::ClientRequestType::FORK) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] FORK" << frigg::EndLog();

		auto action = frigg::compose([=] (auto serialized) {
			StdSharedPtr<Process> new_process = process->fork();

			helx::Directory directory = Process::runServer(new_process);

			HelHandle universe;
			HEL_CHECK(helCreateUniverse(&universe));

			HelHandle thread;
			HEL_CHECK(helCreateThread(universe, new_process->vmSpace, directory.getHandle(),
					kHelAbiSystemV, (void *)request.child_ip(), (void *)request.child_sp(),
					0, &thread));

			managarm::posix::ServerResponse<Allocator> response(*allocator);
			response.set_error(managarm::posix::Errors::SUCCESS);
			response.set_pid(new_process->pid);
			response.SerializeToString(serialized);
			
			return pipe->sendStringResp(serialized->data(), serialized->size(),
					eventHub, msg_request, 0)
			+ frigg::apply([=] (HelError error) { HEL_CHECK(error); });
		}, frigg::String<Allocator>(*allocator));
		
		frigg::run(frigg::move(action), allocator.get());
	}else if(request.request_type() == managarm::posix::ClientRequestType::EXEC) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] EXEC" << frigg::EndLog();

		auto action = frigg::compose([=] (auto serialized) {
			execute(process, request.path());
			
			managarm::posix::ServerResponse<Allocator> response(*allocator);
			response.set_error(managarm::posix::Errors::SUCCESS);
			response.SerializeToString(serialized);
			
			return pipe->sendStringResp(serialized->data(), serialized->size(),
					eventHub, msg_request, 0)
			+ frigg::apply([=] (HelError error) { HEL_CHECK(error); });
		}, frigg::String<Allocator>(*allocator));
		
		frigg::run(frigg::move(action), allocator.get());
	}else if(request.request_type() == managarm::posix::ClientRequestType::FSTAT) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] FSTAT" << frigg::EndLog();
		
		auto file = process->allOpenFiles.get(request.fd());

		auto action = frigg::ifThenElse(
			frigg::apply([=] () { return file; }),

			frigg::await<void(FileStats)>([=] (auto callback) {
				(*file)->fstat(callback);
			})
			+ frigg::compose([=] (FileStats stats, auto serialized) {
				if(traceRequests)
					infoLogger->log() << "[" << process->pid << "] FSTAT response" << frigg::EndLog();

				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::SUCCESS);
				response.set_inode_num(stats.inodeNumber);
				response.set_mode(stats.mode);
				response.set_num_links(stats.numLinks);
				response.set_uid(stats.uid);
				response.set_gid(stats.gid);
				response.set_file_size(stats.fileSize);
				response.set_atime_secs(stats.atimeSecs);
				response.set_atime_nanos(stats.atimeNanos);
				response.set_mtime_secs(stats.mtimeSecs);
				response.set_mtime_nanos(stats.mtimeNanos);
				response.set_ctime_secs(stats.ctimeSecs);
				response.set_ctime_nanos(stats.ctimeNanos);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::apply([=] (HelError error) { HEL_CHECK(error); });
			}, frigg::String<Allocator>(*allocator)),

			frigg::compose([=] (auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::NO_SUCH_FD);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::apply([=] (HelError error) { HEL_CHECK(error); });
			}, frigg::String<Allocator>(*allocator))
		);

		frigg::run(action, allocator.get()); 
	}else if(request.request_type() == managarm::posix::ClientRequestType::OPEN) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] OPEN" << frigg::EndLog();

		// NOTE: this is a hack that works around a segfault in GCC
		auto msg_request2 = msg_request;

		auto action = frigg::await<void(StdSharedPtr<VfsOpenFile> file)>([=] (auto callback) {
			uint32_t open_flags = 0;
			if((request.flags() & managarm::posix::OpenFlags::CREAT) != 0)
				open_flags |= MountSpace::kOpenCreat;

			uint32_t open_mode = 0;
			if((request.mode() & managarm::posix::OpenMode::HELFD) != 0)
				open_mode |= MountSpace::kOpenHelfd;
			
			frigg::String<Allocator> path = concatenatePath("/", request.path());
			frigg::String<Allocator> normalized = normalizePath(path);

			MountSpace *mount_space = process->mountSpace;
			mount_space->openAbsolute(process, frigg::move(normalized), open_flags, open_mode, callback);
		})
		+ frigg::compose([=] (StdSharedPtr<VfsOpenFile> file) {
			return frigg::ifThenElse(
				frigg::apply([=] () {
					// NOTE: this is a hack that works around a bug in GCC
					auto f = file;
					return (bool)f;
				}),

				frigg::compose([=] (auto serialized) {
					int fd = process->nextFd;
					assert(fd > 0);
					process->nextFd++;
					process->allOpenFiles.insert(fd, frigg::move(file));

					if(traceRequests)
						infoLogger->log() << "[" << process->pid << "] OPEN response" << frigg::EndLog();

					managarm::posix::ServerResponse<Allocator> response(*allocator);
					response.set_error(managarm::posix::Errors::SUCCESS);
					response.set_fd(fd);
					response.SerializeToString(serialized);
					
					return pipe->sendStringResp(serialized->data(), serialized->size(),
							eventHub, msg_request2, 0)
					+ frigg::apply([=] (HelError error) { HEL_CHECK(error); });
				}, frigg::String<Allocator>(*allocator)),

				frigg::compose([=] (auto serialized) {
					managarm::posix::ServerResponse<Allocator> response(*allocator);
					response.set_error(managarm::posix::Errors::FILE_NOT_FOUND);
					return pipe->sendStringResp(serialized->data(), serialized->size(),
							eventHub, msg_request, 0)
					+ frigg::apply([=] (HelError error) { HEL_CHECK(error); });
				}, frigg::String<Allocator>(*allocator))
			);
		});
		
		frigg::run(action, allocator.get());
	}else if(request.request_type() == managarm::posix::ClientRequestType::CONNECT) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] CONNECT" << frigg::EndLog();
	
		auto file = process->allOpenFiles.get(request.fd());
	
		auto action = frigg::ifThenElse(
			frigg::apply([=] () { return file; }),

			frigg::await<void()>([=] (auto callback) {
				(*file)->connect(callback);
			})
			+ frigg::compose([=] (auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::SUCCESS);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::apply([=] (HelError error) { HEL_CHECK(error); });
			}, frigg::String<Allocator>(*allocator)),

			frigg::compose([=] (auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::NO_SUCH_FD);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::apply([=] (HelError error) { HEL_CHECK(error); });
			}, frigg::String<Allocator>(*allocator))
		);

		frigg::run(action, allocator.get());
	}else if(request.request_type() == managarm::posix::ClientRequestType::WRITE) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] WRITE" << frigg::EndLog();

			auto file = process->allOpenFiles.get(request.fd());
			
			auto action = frigg::ifThenElse(
				frigg::apply([=] () { return file; }),

				frigg::await<void()>([=] (auto callback) {
					(*file)->write(request.buffer().data(), request.buffer().size(), callback);
				})
				+ frigg::compose([=] (auto serialized) {
					managarm::posix::ServerResponse<Allocator> response(*allocator);
					response.set_error(managarm::posix::Errors::SUCCESS);
					response.SerializeToString(serialized);

					return pipe->sendStringResp(serialized->data(), serialized->size(),
							eventHub, msg_request, 0)
					+ frigg::apply([=] (HelError error) { HEL_CHECK(error); });
				}, frigg::String<Allocator>(*allocator)),
				
				frigg::compose([=] (auto serialized) {
					managarm::posix::ServerResponse<Allocator> response(*allocator);
					response.set_error(managarm::posix::Errors::NO_SUCH_FD);
					response.SerializeToString(serialized);

					return pipe->sendStringResp(serialized->data(), serialized->size(),
								eventHub, msg_request, 0)
					+ frigg::apply([=] (HelError error) { HEL_CHECK(error); });
				}, frigg::String<Allocator>(*allocator))
			);

			frigg::run(action, allocator.get());
	}else if(request.request_type() == managarm::posix::ClientRequestType::READ) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] READ" << frigg::EndLog();

		auto file = process->allOpenFiles.get(request.fd());

		auto action = frigg::ifThenElse(
			frigg::apply([=] () { return file; }),

			frigg::compose([=] (frigg::String<Allocator> *buffer) {
				return frigg::await<void(VfsError error, size_t actual_size)>([=] (auto callback) {
					buffer->resize(request.size());
					(*file)->read(buffer->data(), request.size(), callback);
				})
				+ frigg::compose([=] (VfsError error, size_t actual_size) {
					// FIXME: hack to work around a GCC bug
					auto msg_request2 = msg_request;
					
					return frigg::ifThenElse(
						frigg::apply([=] () { return error == kVfsEndOfFile; }),

						frigg::compose([=] (auto serialized) {
							managarm::posix::ServerResponse<Allocator> response(*allocator);
							response.set_error(managarm::posix::Errors::END_OF_FILE);
							response.SerializeToString(serialized);
							
							return pipe->sendStringResp(serialized->data(), serialized->size(),
									eventHub, msg_request2, 0)
							+ frigg::apply([=] (HelError error) { HEL_CHECK(error); });
						}, frigg::String<Allocator>(*allocator)),

						frigg::compose([=] (auto serialized) {
							assert(error == kVfsSuccess);
							
							// TODO: make request.size() unsigned
							managarm::posix::ServerResponse<Allocator> response(*allocator);
							response.set_error(managarm::posix::Errors::SUCCESS);
							response.SerializeToString(serialized);

							return pipe->sendStringResp(serialized->data(), serialized->size(),
									eventHub, msg_request2, 0)
							+ frigg::apply([=] (HelError error) { HEL_CHECK(error); })
							+ pipe->sendStringResp(buffer->data(), actual_size,
									eventHub, msg_request2, 1)
							+ frigg::apply([=] (HelError error) { HEL_CHECK(error); });
						}, frigg::String<Allocator>(*allocator))
					);
				});
			}, frigg::String<Allocator>(*allocator)),

			frigg::compose([=] (auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::NO_SUCH_FD);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::apply([=] (HelError error) { HEL_CHECK(error); });
			}, frigg::String<Allocator>(*allocator))
		);

		frigg::run(action, allocator.get());
	}else if(request.request_type() == managarm::posix::ClientRequestType::SEEK_ABS
			|| request.request_type() == managarm::posix::ClientRequestType::SEEK_REL
			|| request.request_type() == managarm::posix::ClientRequestType::SEEK_EOF) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] SEEK" << frigg::EndLog();

		auto file = process->allOpenFiles.get(request.fd());

		auto action = frigg::ifThenElse(
			frigg::apply([=] () { return file; }),

			frigg::await<void(uint64_t offset)>([=] (auto callback) {
				if(request.request_type() == managarm::posix::ClientRequestType::SEEK_ABS) {
					(*file)->seek(request.rel_offset(), kSeekAbs, callback);
				}else if(request.request_type() == managarm::posix::ClientRequestType::SEEK_REL) {
					(*file)->seek(request.rel_offset(), kSeekRel, callback);
				}else if(request.request_type() == managarm::posix::ClientRequestType::SEEK_EOF) {
					(*file)->seek(request.rel_offset(), kSeekEof, callback);
				}else{
					frigg::panicLogger.log() << "Illegal SEEK request" << frigg::EndLog();
				}
			})

			+ frigg::compose([=] (uint64_t offset, auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::SUCCESS);
				response.set_offset(offset);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::apply([=] (HelError error) { HEL_CHECK(error); });
			}, frigg::String<Allocator>(*allocator)),

			frigg::compose([=] (auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::NO_SUCH_FD);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::apply([=] (HelError error) { HEL_CHECK(error); });
			}, frigg::String<Allocator>(*allocator))
		);

		frigg::run(action, allocator.get());
	}else if(request.request_type() == managarm::posix::ClientRequestType::MMAP) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] MMAP" << frigg::EndLog();

		auto file = process->allOpenFiles.get(request.fd());
		
		auto action = frigg::ifThenElse(
			frigg::apply([=] () { return file; }),

			frigg::await<void(HelHandle handle)>([=] (auto callback) {
				(*file)->mmap(callback);
			})
			+ frigg::compose([=] (HelHandle handle, auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::SUCCESS);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::apply([=] (HelError error) { 
					HEL_CHECK(error);
				})
				+ pipe->sendDescriptorResp(handle, eventHub, msg_request, 1)
				+ frigg::apply([=] (HelError error) {
					HEL_CHECK(error);
					HEL_CHECK(helCloseDescriptor(handle));
				});
			}, frigg::String<Allocator>(*allocator)),

			frigg::compose([=] (auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::NO_SUCH_FD);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::apply([=] (HelError error) { HEL_CHECK(error);	});
			}, frigg::String<Allocator>(*allocator))
		);

		frigg::run(action, allocator.get());
	}else if(request.request_type() == managarm::posix::ClientRequestType::CLOSE) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] CLOSE" << frigg::EndLog();

		
		auto action = frigg::compose([=] (auto serialized) {
			managarm::posix::ServerResponse<Allocator> response(*allocator);

			int32_t fd = request.fd();
			auto file_wrapper = process->allOpenFiles.get(fd);
			if(file_wrapper){
				process->allOpenFiles.remove(fd);
				response.set_error(managarm::posix::Errors::SUCCESS);
			}else{
				response.set_error(managarm::posix::Errors::NO_SUCH_FD);
			}
			
			response.SerializeToString(serialized);
			
			return pipe->sendStringResp(serialized->data(), serialized->size(),
					eventHub, msg_request, 0)
			+ frigg::apply([=] (HelError error) { HEL_CHECK(error); });
		}, frigg::String<Allocator>(*allocator));
		
		frigg::run(frigg::move(action), allocator.get());
	}else if(request.request_type() == managarm::posix::ClientRequestType::DUP2) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] DUP2" << frigg::EndLog();


		auto action = frigg::compose([=] (auto serialized) {
			managarm::posix::ServerResponse<Allocator> response(*allocator);

			int32_t oldfd = request.fd();
			int32_t newfd = request.newfd();
			auto file_wrapper = process->allOpenFiles.get(oldfd);
			if(file_wrapper){
				auto file = *file_wrapper;
				process->allOpenFiles.insert(newfd, file);

				response.set_error(managarm::posix::Errors::SUCCESS);
			}else{
				response.set_error(managarm::posix::Errors::NO_SUCH_FD);
			}
			response.SerializeToString(serialized);
			
			return pipe->sendStringResp(serialized->data(), serialized->size(),
					eventHub, msg_request, 0)
			+ frigg::apply([=] (HelError error) { HEL_CHECK(error); });
		}, frigg::String<Allocator>(*allocator));
		
		frigg::run(frigg::move(action), allocator.get());
	}else if(request.request_type() == managarm::posix::ClientRequestType::TTY_NAME) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] TTY_NAME" << frigg::EndLog();

		auto file = process->allOpenFiles.get(request.fd());
		
		auto action = frigg::ifThenElse(
			frigg::apply([=] () { return file; }),

			frigg::compose([=] (auto serialized) {
				frigg::Optional<frigg::String<Allocator>> result = (*file)->ttyName();
				
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				if(result) {
					response.set_error(managarm::posix::Errors::SUCCESS);
					response.set_path(*result);
				}else{
					response.set_error(managarm::posix::Errors::BAD_FD);
				}
				
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::apply([=] (HelError error) { 
					HEL_CHECK(error);	
				});
			}, frigg::String<Allocator>(*allocator)),
			
			frigg::compose([=] (auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::NO_SUCH_FD);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::apply([=] (HelError error) { 
					HEL_CHECK(error);	
				});
			}, frigg::String<Allocator>(*allocator))
		);
			
		frigg::run(action, allocator.get());
	}else if(request.request_type() == managarm::posix::ClientRequestType::HELFD_ATTACH) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] HELFD_ATTACH" << frigg::EndLog();

		HelError error;
		HelHandle handle;
		//FIXME
		pipe->recvDescriptorReqSync(eventHub, msg_request, 1, error, handle);
		HEL_CHECK(error);

		auto file_wrapper = process->allOpenFiles.get(request.fd());
		
		auto action = frigg::ifThenElse(
			frigg::apply([=] () { return file_wrapper; }),

			frigg::compose([=] (auto serialized) {
				auto file = *file_wrapper;
				file->setHelfd(handle);
				
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::SUCCESS);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::apply([=] (HelError error) { 
					HEL_CHECK(error);	
				});
			}, frigg::String<Allocator>(*allocator)),

			frigg::compose([=] (auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::NO_SUCH_FD);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::apply([=] (HelError error) { 
					HEL_CHECK(error);	
				});
			}, frigg::String<Allocator>(*allocator))
		);

		frigg::run(frigg::move(action), allocator.get());
	}else if(request.request_type() == managarm::posix::ClientRequestType::HELFD_CLONE) {
		if(traceRequests)
			infoLogger->log() << "[" << process->pid << "] HELFD_CLONE" << frigg::EndLog();

		auto file_wrapper = process->allOpenFiles.get(request.fd());
		
		auto action = frigg::ifThenElse(
			frigg::apply([=] () { return file_wrapper; }),

			frigg::compose([=] (auto serialized) {
				auto file = *file_wrapper;
				infoLogger->log() << "[posix/subsystem/src/main] HELFD_CLONE sendDescriptorResp" << frigg::EndLog();
				pipe->sendDescriptorResp(file->getHelfd(), msg_request, 1);
				
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::SUCCESS);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::apply([=] (HelError error) { 
					HEL_CHECK(error);	
				});
			}, frigg::String<Allocator>(*allocator)),

			frigg::compose([=] (auto serialized) {
				managarm::posix::ServerResponse<Allocator> response(*allocator);
				response.set_error(managarm::posix::Errors::NO_SUCH_FD);
				response.SerializeToString(serialized);
				
				return pipe->sendStringResp(serialized->data(), serialized->size(),
						eventHub, msg_request, 0)
				+ frigg::apply([=] (HelError error) { 
					HEL_CHECK(error);	
				});
			}, frigg::String<Allocator>(*allocator))
		);

		frigg::run(frigg::move(action), allocator.get());
	}else{
		auto action = frigg::compose([=] (auto serialized) {
			managarm::posix::ServerResponse<Allocator> response(*allocator);
			response.set_error(managarm::posix::Errors::ILLEGAL_REQUEST);
			response.SerializeToString(serialized);
			
			return pipe->sendStringResp(serialized->data(), serialized->size(),
					eventHub, msg_request, 0)
			+ frigg::apply([=] (HelError error) { 
				HEL_CHECK(error);	
			});
		}, frigg::String<Allocator>(*allocator));

		frigg::run(frigg::move(action), allocator.get());
	}
}

void RequestClosure::operator() () {
	HelError error = pipe->recvStringReqToRing(ringBuffer, eventHub, kHelAnyRequest, 0,
			CALLBACK_MEMBER(this, &RequestClosure::recvRequest));
	if(error == kHelErrClosedRemotely) {
		suicide(*allocator);
		return;
	}
	HEL_CHECK(error);
}

void RequestClosure::recvRequest(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t index, size_t offset, size_t length) {
	if(error == kHelErrClosedRemotely) {
		suicide(*allocator);
		return;
	}
	HEL_CHECK(error);

	managarm::posix::ClientRequest<Allocator> request(*allocator);
	request.ParseFromArray(ringItem->data + offset, length);
	processRequest(frigg::move(request), msg_request);

	(*this)();
}

// --------------------------------------------------------
// AcceptClosure
// --------------------------------------------------------

struct AcceptClosure : frigg::BaseClosure<AcceptClosure> {
public:
	AcceptClosure(helx::Server server, frigg::SharedPtr<Process> process, int iteration);

	void operator() ();

private:
	void accepted(HelError error, HelHandle handle);

	helx::Server p_server;
	frigg::SharedPtr<Process> process;
	int iteration;
};

AcceptClosure::AcceptClosure(helx::Server server, frigg::SharedPtr<Process> process, int iteration)
: p_server(frigg::move(server)), process(frigg::move(process)), iteration(iteration) { }

void AcceptClosure::operator() () {
	p_server.accept(eventHub, CALLBACK_MEMBER(this, &AcceptClosure::accepted));
}

void AcceptClosure::accepted(HelError error, HelHandle handle) {
	HEL_CHECK(error);
	
	auto pipe = frigg::makeShared<helx::Pipe>(*allocator, handle);
	frigg::runClosure<RequestClosure>(*allocator, frigg::move(pipe), process, iteration);
	(*this)();
}

void acceptLoop(helx::Server server, StdSharedPtr<Process> process, int iteration) {
	frigg::runClosure<AcceptClosure>(*allocator, frigg::move(server),
			frigg::move(process), iteration);
}

// --------------------------------------------------------
// QueryDeviceIfClosure
// --------------------------------------------------------

struct QueryDeviceIfClosure {
	QueryDeviceIfClosure(int64_t request_id);

	void operator() ();

private:
	void recvdPipe(HelError error, int64_t msg_request, int64_t msg_seq, HelHandle handle);

	int64_t requestId;
};

QueryDeviceIfClosure::QueryDeviceIfClosure(int64_t request_id)
: requestId(request_id) { }

void QueryDeviceIfClosure::operator() () {
	mbusPipe.recvDescriptorResp(eventHub, requestId, 1,
			CALLBACK_MEMBER(this, &QueryDeviceIfClosure::recvdPipe));
}

void QueryDeviceIfClosure::recvdPipe(HelError error, int64_t msg_request, int64_t msq_seq,
		HelHandle handle) {
	auto fs = frigg::construct<extern_fs::MountPoint>(*allocator, helx::Pipe(handle));
	if(requestId == 1) { // FIXME: UGLY HACK
		frigg::infoLogger.log() << "/ is ready!" << frigg::EndLog();
		auto path = frigg::String<Allocator>(*allocator, "");
		initMountSpace->allMounts.insert(path, fs);
	}else if(requestId == 2) {
		frigg::infoLogger.log() << "/dev/network is ready!" << frigg::EndLog();
		auto path = frigg::String<Allocator>(*allocator, "/dev/network");
		initMountSpace->allMounts.insert(path, fs);
	}else{
		frigg::panicLogger.log() << "Unexpected requestId" << frigg::EndLog();
	}
}

// --------------------------------------------------------
// MbusClosure
// --------------------------------------------------------

struct MbusClosure : public frigg::BaseClosure<MbusClosure> {
	void operator() ();

private:
	void recvdBroadcast(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

	uint8_t buffer[128];
};

void MbusClosure::operator() () {
	HEL_CHECK(mbusPipe.recvStringReq(buffer, 128, eventHub, kHelAnyRequest, 0,
			CALLBACK_MEMBER(this, &MbusClosure::recvdBroadcast)));
}

bool hasCapability(const managarm::mbus::SvrRequest<Allocator> &svr_request,
		frigg::StringView name) {
	for(size_t i = 0; i < svr_request.caps_size(); i++)
		if(svr_request.caps(i).name() == name)
			return true;
	return false;
}

void MbusClosure::recvdBroadcast(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	managarm::mbus::SvrRequest<Allocator> svr_request(*allocator);
	svr_request.ParseFromArray(buffer, length);

	infoLogger->log() << "[posix/subsystem/src/main] recvdBroadcast" << frigg::EndLog();
	if(hasCapability(svr_request, "file-system")) {
		managarm::mbus::CntRequest<Allocator> request(*allocator);
		request.set_req_type(managarm::mbus::CntReqType::QUERY_IF);
		request.set_object_id(svr_request.object_id());

		frigg::String<Allocator> serialized(*allocator);
		request.SerializeToString(&serialized);

		infoLogger->log() << "[posix/subsystem/src/main] file-system sendStringReq" << frigg::EndLog();
		mbusPipe.sendStringReq(serialized.data(), serialized.size(), 1, 0);

		frigg::runClosure<QueryDeviceIfClosure>(*allocator, 1);
	}else if(hasCapability(svr_request, "network")) {
		managarm::mbus::CntRequest<Allocator> request(*allocator);
		request.set_req_type(managarm::mbus::CntReqType::QUERY_IF);
		request.set_object_id(svr_request.object_id());

		frigg::String<Allocator> serialized(*allocator);
		request.SerializeToString(&serialized);
		infoLogger->log() << "[posix/subsystem/src/main] network sendStringReq" << frigg::EndLog();
		mbusPipe.sendStringReq(serialized.data(), serialized.size(), 2, 0);

		frigg::runClosure<QueryDeviceIfClosure>(*allocator, 2);
	}

	(*this)();
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

typedef void (*InitFuncPtr) ();
extern InitFuncPtr __init_array_start[];
extern InitFuncPtr __init_array_end[];

int main() {
	infoLogger.initialize(infoSink);
	infoLogger->log() << "Starting posix-subsystem" << frigg::EndLog();
	allocator.initialize(virtualAlloc);

	// we're using no libc, so we have to run constructors manually
	size_t init_count = __init_array_end - __init_array_start;
	for(size_t i = 0; i < init_count; i++)
		__init_array_start[i]();
	
	ringItem = (HelRingBuffer *)allocator->allocate(sizeof(HelRingBuffer) + 0x10000);
	
	// initialize our string queue
	HEL_CHECK(helCreateRing(0x1000, &ringBuffer));
	int64_t async_id;
	HEL_CHECK(helSubmitRing(ringBuffer, eventHub.getHandle(),
			ringItem, 0x10000, 0, 0, &async_id));

	// connect to mbus
	const char *mbus_path = "local/mbus";
	HelHandle mbus_handle;
	HEL_CHECK(helRdOpen(mbus_path, strlen(mbus_path), &mbus_handle));
	mbusConnect = helx::Client(mbus_handle);
	
	HelError mbus_connect_error;
	mbusConnect.connectSync(eventHub, mbus_connect_error, mbusPipe);
	HEL_CHECK(mbus_connect_error);

	// enumerate the initrd object
	managarm::mbus::CntRequest<Allocator> enum_request(*allocator);
	enum_request.set_req_type(managarm::mbus::CntReqType::ENUMERATE);
	
	managarm::mbus::Capability<Allocator> cap(*allocator);
	cap.set_name(frigg::String<Allocator>(*allocator, "initrd"));
	enum_request.add_caps(frigg::move(cap));

	HelError enumerate_error;
	frigg::String<Allocator> enum_serialized(*allocator);
	enum_request.SerializeToString(&enum_serialized);
	mbusPipe.sendStringReqSync(enum_serialized.data(), enum_serialized.size(),
			eventHub, 0, 0, enumerate_error);

	uint8_t enum_buffer[128];
	HelError enum_error;
	size_t enum_length;
	mbusPipe.recvStringRespSync(enum_buffer, 128, eventHub, 0, 0, enum_error, enum_length);
	HEL_CHECK(enum_error);
	
	managarm::mbus::SvrResponse<Allocator> enum_response(*allocator);
	enum_response.ParseFromArray(enum_buffer, enum_length);
	
	// query the initrd object
	managarm::mbus::CntRequest<Allocator> query_request(*allocator);
	query_request.set_req_type(managarm::mbus::CntReqType::QUERY_IF);
	query_request.set_object_id(enum_response.object_id());

	HelError send_query_error;
	frigg::String<Allocator> query_serialized(*allocator);
	query_request.SerializeToString(&query_serialized);
	mbusPipe.sendStringReqSync(query_serialized.data(), query_serialized.size(),
			eventHub, 0, 0, send_query_error);
	
	HelError recv_query_error;
	HelHandle query_handle;
	mbusPipe.recvDescriptorRespSync(eventHub, 0, 1, recv_query_error, query_handle);
	HEL_CHECK(recv_query_error);
	initrdPipe = helx::Pipe(query_handle);

	frigg::runClosure<MbusClosure>(*allocator);

	// start our own server
	helx::Server server;
	helx::Client client;
	helx::Server::createServer(server, client);
	acceptLoop(frigg::move(server), StdSharedPtr<Process>(), 0);

	const char *parent_path = "local/parent";
	HelHandle parent_handle;
	HEL_CHECK(helRdOpen(parent_path, strlen(parent_path), &parent_handle));
	
	helx::Pipe parent_pipe(parent_handle);
	HelError send_error;
	parent_pipe.sendDescriptorSync(client.getHandle(), eventHub, 0, 0, 
			kHelRequest, send_error);
	HEL_CHECK(send_error);

	parent_pipe.reset();
	client.reset();
	
	while(true) {
		eventHub.defaultProcessEvents();
	}
}

asm ( ".global _start\n"
		"_start:\n"
		"\tcall main\n"
		"\tud2" );

extern "C"
int __cxa_atexit(void (*func) (void *), void *arg, void *dso_handle) {
	return 0;
}

void *__dso_handle;

