#include <cstring>
#include "../vp8/util/memory.hh"
#include "smalljpg.hh"
#include "jpgcoder.hh"
#ifdef _WIN32
#include <io.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <signal.h>
#include <thread>
#include "../vp8/util/nd_array.hh"
#include "../io/MuxReader.hh"
#include "../io/ioutil.hh"
#ifdef USE_SYSTEM_MD5_DEPENDENCY
#include <openssl/md5.h>
#else
#include "../../dependencies/md5/md5.h"
#endif

extern int app_main(int argc, char ** argv);
static const char cb64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

struct B64lut {
    unsigned char lut[256];
    B64lut() {
        memset(lut, 0, sizeof(lut));
        for (int i = 0; i < 64; ++i) {
            lut[(size_t)cb64[i]] = i;
        }
    }
};
B64lut b64lut;
void decode_4_to_3(unsigned char* out,
                   unsigned char inp0, unsigned char inp1, unsigned char inp2, unsigned char inp3) {
    unsigned char in0 = b64lut.lut[inp0];
    unsigned char in1 = b64lut.lut[inp1];
    unsigned char in2 = b64lut.lut[inp2];
    unsigned char in3 = b64lut.lut[inp3];
    out[0] = (in0 << 2) | (in1 >> 4);
    out[1] = (in1 << 4) | (in2 >> 2);
    out[2] = ((in2 << 6) & 0xc0) | in3;
}
void decode_in_place(unsigned char *data, size_t size) {
    size_t j = 0;
    size_t i = 0;
    
    for (i = 3; i < size; i += 4, j += 3) {
        decode_4_to_3(data + j, data[i - 3], data[i - 2], data[i - 1], data[i]);
    }
    for (i = (size - (size & 3)); i < size; i += 4, j += 3) {
        decode_4_to_3(data + j,
                      data[i],
                      i + 1 < size ? data[i + 1] : '=',
                      i + 2 < size ? data[i + 2] : '=',
                      i + 3 < size ? data[i + 3] : '=');
    }
}
int run_benchmark(char * argv0, unsigned char *file, size_t file_size);
#ifdef REALISTIC_BENCHMARK
extern unsigned char benchmark_file[3560812];
#endif
int benchmark(int argc, char ** argv) {
    const char * filename = NULL;
    for (int i = 1; i < argc; ++i){
        if (argv[i][0] != '-') {
            filename = argv[i];
            break;
        }
    }

    std::vector<unsigned char> data;
    if (filename != NULL) {
        FILE * fp = fopen(filename, "rb");
        if (!fp) {
            fprintf(stderr, "%s not found...\n", filename);
            custom_exit(ExitCode::FILE_NOT_FOUND);
        }
        fseek(fp, 0, SEEK_END);
        size_t file_size = (size_t)ftell(fp);
        fseek(fp, 0, SEEK_SET);
        data.resize(file_size);
        if (file_size) {
            auto ret = fread(&data[0], file_size, 1, fp);
            always_assert(ret);
        }
        fclose(fp);
    } else {
        data.resize(bigger_hdr_len + bigger_rep_len * bigger_num_reps);
        memcpy(&data[0], bigger_hdr, bigger_hdr_len);
        for (int i = 0; i < bigger_num_reps; ++i) {
            memcpy(&data[bigger_hdr_len + i * bigger_rep_len], bigger_rep, bigger_rep_len);
        }
    }
    return run_benchmark(argv[0], &data[0], data.size());
}
size_t args_size(const char **args) {
    size_t retval = 0;
    while(args[retval]) {
        ++retval;
    }
    return retval;
}
class MemReader : public Sirikata::DecoderReader{
    const unsigned char *begin;
    const unsigned char *end;
public:
    MemReader(const unsigned char *b, const unsigned char*e) {
        begin = b;
        end = e;
    }
    std::pair<Sirikata::uint32, Sirikata::JpegError> Read(Sirikata::uint8*data, unsigned int size) {
        using namespace Sirikata;
        size_t bytesLeft = end - begin;
        size_t actualBytesRead = size;
        if (bytesLeft < size) {
            actualBytesRead = bytesLeft;
        }
        if (actualBytesRead > 0) {
            memcpy(data, begin, actualBytesRead);
        }
        begin += actualBytesRead;
        JpegError err = JpegError();
        if (actualBytesRead == 0) {
            err = JpegError::errEOF();
        }
        std::pair<Sirikata::uint32, JpegError> retval(static_cast<Sirikata::uint32>(actualBytesRead), err);
        return retval;
    }
};
void blind_write_to_pipe(int pipe, const unsigned char * file, size_t file_size) {
    while(file_size) {
        auto ret = write(pipe, file, std::min(file_size, (size_t)65536));
        if (ret == -1) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        file_size -= ret;
        file += ret;
    }
}
Sirikata::Array1d<uint8_t, 16> do_first_encode(const unsigned char * file, size_t file_size, const char ** options,
                     Sirikata::MuxReader::ResizableByteBuffer*out) {
    auto encode_pipes = IOUtil::start_subprocess(args_size(options), &options[0], false);
    bool is_socket = false;
    if (out) {
        out->reserve(file_size);
    }
    size_t size = 0;
    size_t start_byte = 0;
    size_t end_byte = file_size;
    int reader[2];
    while (pipe(reader) < 0) {
        always_assert(errno == EINTR);
    }
    std::thread wtp(std::bind(&blind_write_to_pipe, reader[1], file + 2, file_size - 2));
    Sirikata::Array1d<uint8_t, 16> md5 = IOUtil::transfer_and_md5({{file[0], file[1]}},
                                                                  start_byte,
                                                                  end_byte,
                                                                  true,
                                                                  reader[0],
                                                                  encode_pipes.pipe_stdin,
                                                                  encode_pipes.pipe_stdout,
                                                                  &size,
                                                                  out,
                                                                  is_socket);
    wtp.join();
   return md5;
}
IOUtil::SubprocessConnection safe_start_subprocess(int argc, const char **argv, bool pipe_stderr) {
    return IOUtil::start_subprocess(argc, argv, pipe_stderr);
}
void do_code(const unsigned char * file, size_t file_size, const char ** options, Sirikata::Array1d<uint8_t, 16> md5, int reps) {
    for (int rep = 0; rep < reps; ++rep) {
        auto decode_pipes = IOUtil::start_subprocess(args_size(options), (const char**)&options[0], false);
        size_t roundtrip_size = 0;
        Sirikata::Array1d<uint8_t, 16> rtmd5;
        rtmd5 = IOUtil::send_and_md5_result(
            file,
            file_size,
            decode_pipes.pipe_stdin,
            decode_pipes.pipe_stdout,
            &roundtrip_size);
        if (memcmp(&md5[0], &rtmd5[0], md5.size()) != 0) {
            fprintf(stderr, "Input Size %lu != Roundtrip Size %lu\n", (unsigned long)file_size, (unsigned long)roundtrip_size);
            for (size_t i = 0; i < md5.size(); ++i) {
                fprintf(stderr, "%02x", md5[i]);
        }
            fprintf(stderr, " != ");
            for (size_t i = 0; i < rtmd5.size(); ++i) {
                fprintf(stderr, "%02x", rtmd5[i]);
            }
            fprintf(stderr, "\n");
            fflush(stderr);
            abort();
        }
    }
}
struct TestOptions {
    int parallel_encodes;
    int parallel_decodes;
    int reps;
    int barrier_reps;
    bool flush_workers;
    TestOptions(){
        parallel_encodes = 1;
        parallel_decodes = 1;
        reps = 1;
        barrier_reps = 1;
        flush_workers = true;
    }
};
double do_benchmark(TestOptions test,
                    unsigned char * file,
                    size_t file_size,
                    const char ** enc_options, const char ** dec_options = NULL) {
    Sirikata::MuxReader::ResizableByteBuffer encoded_file;
    auto file_md5 = do_first_encode(file, file_size, enc_options, &encoded_file);
    
    Sirikata::Array1d<uint8_t, 16> encoded_md5;
    {
        MD5_CTX context;
        MD5_Init(&context);
        MD5_Update(&context, &encoded_file[0], encoded_file.size());
        MD5_Final(&encoded_md5[0], &context);
        MD5_Init(&context);
        MD5_Update(&context, &file[0], file_size);
        Sirikata::Array1d<uint8_t, 16> doubleCheckInput;
        MD5_Final(&doubleCheckInput[0], &context);
        if (memcmp(&doubleCheckInput[0], &file_md5[0], 16) != 0) {
            for (size_t i = 0; i < doubleCheckInput.size(); ++i) {
                fprintf(stderr, "%02x", doubleCheckInput[i]);
            }
            fprintf(stderr, " != ");
            for (size_t i = 0; i < file_md5.size(); ++i) {
                fprintf(stderr, "%02x", file_md5[i]);
            }
            fprintf(stderr, "\n");
            abort();
        }
    }
    if (dec_options == NULL) {
        dec_options = enc_options;
    }
    std::vector<std::thread *>workers;
    double start = TimingHarness::get_time_us(true);
    for (int b_rep = 0; b_rep < test.barrier_reps; ++b_rep) {
        if (test.parallel_encodes) {
            for (int i = 0;i < (test.parallel_decodes ==0 ? test.parallel_encodes - 1 : test.parallel_encodes); ++i) {
                workers.push_back(new std::thread(std::bind(&do_code, file, file_size, enc_options, encoded_md5, test.reps)));
            }
            if (test.parallel_decodes == 0) {
                do_code(file, file_size, enc_options, encoded_md5, test.reps);
            }
        }
        if (test.parallel_decodes) {
            for (int i = 0;i < test.parallel_decodes - 1; ++i) {
                workers.push_back(new std::thread(std::bind(&do_code, &encoded_file[0], encoded_file.size(), dec_options, file_md5, test.reps)));
            }
            do_code(&encoded_file[0], encoded_file.size(), dec_options, file_md5, test.reps);
        }
        if (test.flush_workers)  {
            for (auto th : workers) {
                th->join();
                delete th;
            }
            workers.resize(0);
        }
    }
    double end = TimingHarness::get_time_us(true);
    if (!test.flush_workers)  {
        for (auto th : workers) {
            th->join();
            delete th;
        }
        workers.resize(0);
    }
    return (end - start) / 1000000. / test.barrier_reps / test.reps;
}
std::string itoas(int number) {
    char data[128] = {0};
    sprintf(data, "%d", number);
    return data;
}
void print_results(int num_ops, const std::string &name, size_t file_size, double total_time) {
    fprintf(stdout, "%s: %.2fms (%.2fMbit/s)\n",
            name.c_str(),
            total_time * 1000,
            file_size * 8 * double(num_ops) / total_time / 1024 / 1024);
}
int run_benchmark(char * argv0, unsigned char *file, size_t file_size) {
    const char* options[] = {argv0, "-", NULL};
    const char* options_1way[] = {argv0, "-", "-singlethread", NULL};
    const char* skip_verify[] = {argv0, "-", "-skipverify", NULL};
    const int parallel_latency_tests[] = {2, 4, 6, 8, 12, 16};
    double total_time = 0;
    TestOptions test;
    test.reps = 8;
    total_time = do_benchmark(test ,file, file_size, options);
    print_results(2, "Verified encode followed by decode", file_size, total_time);
    test.parallel_encodes = 1;
    test.parallel_decodes = 0;
    total_time = do_benchmark(test, file, file_size, options);
    print_results(1, "Verified encode", file_size, total_time);
    total_time = do_benchmark(test, file, file_size, skip_verify);
    print_results(1, "Unverified encode", file_size, total_time);
    test.parallel_encodes = 0;
    test.parallel_decodes = 1;
    total_time = do_benchmark(test ,file, file_size, options);
    print_results(1, "decode", file_size, total_time);
    test.flush_workers = false;
    test.reps /= 2;
    for (size_t i = 0; i < sizeof(parallel_latency_tests)/sizeof(parallel_latency_tests[0]); ++i) {
        int p = parallel_latency_tests[i];
        test.parallel_encodes = p;
        test.parallel_decodes = 0;
    
        total_time = do_benchmark(test,file, file_size, options);
        print_results(1, "Loaded " + itoas(p) +" Verified encode", file_size, total_time);
    
        total_time = do_benchmark(test, file, file_size, skip_verify);
        print_results(1, "Loaded "  + itoas(p) + " Unverified encode", file_size, total_time);
        test.parallel_encodes = 0;
        test.parallel_decodes = p;
        total_time = do_benchmark(test,file, file_size, options);
        print_results(1, "Loaded " + itoas(p) + " decode", file_size, total_time);
    }
    test.flush_workers = true;
    test.parallel_encodes = 1;
    test.parallel_decodes = 1;
    total_time = do_benchmark(test,file, file_size, options_1way);
    print_results(2, "1-way encode followed by decode", file_size, total_time);
    test.parallel_encodes = 1;
    test.parallel_decodes = 0;
    total_time = do_benchmark(test,file, file_size, options_1way);
    print_results(1, "1-way encode", file_size, total_time);
    test.parallel_encodes = 0;
    test.parallel_decodes = 1;
    total_time = do_benchmark(test,file, file_size, options_1way);
    print_results(1, "1-way decode", file_size, total_time);

    return 0;
}
