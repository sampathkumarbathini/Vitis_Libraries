#include <ap_int.h>
#include <iostream>

#include <openssl/rc4.h>
#include <openssl/evp.h>

#include <sys/time.h>
#include <new>
#include <cstdlib>

#include <xcl2.hpp>

// maximum text length in 512-bit
// XXX BUFFER_SIZE = MAX_ROW * 64 Bytes
#define MAX_ROW 1048576
// text length for each task in byte
#define N_ROW 2048
//#define N_ROW 2097152
// number of tasks for a single PCIe block
#define N_TASK 2
// number of PUs
// should be a multiple of 4, and 4 <= CH_NM <= 64
#define CH_NM 8
// cipher key size in byte
#define KEY_SIZE 32

class ArgParser {
   public:
    ArgParser(int& argc, const char** argv) {
        for (int i = 1; i < argc; ++i) mTokens.push_back(std::string(argv[i]));
    }
    bool getCmdOption(const std::string option, std::string& value) const {
        std::vector<std::string>::const_iterator itr;
        itr = std::find(this->mTokens.begin(), this->mTokens.end(), option);
        if (itr != this->mTokens.end() && ++itr != this->mTokens.end()) {
            value = *itr;
            return true;
        }
        return false;
    }

   private:
    std::vector<std::string> mTokens;
};

inline int tvdiff(struct timeval* tv0, struct timeval* tv1) {
    return (tv1->tv_sec - tv0->tv_sec) * 1000000 + (tv1->tv_usec - tv0->tv_usec);
}

template <typename T>
T* aligned_alloc(std::size_t num) {
    void* ptr = nullptr;
    if (posix_memalign(&ptr, 4096, num * sizeof(T))) throw std::bad_alloc();
    return reinterpret_cast<T*>(ptr);
}

int main(int argc, char* argv[]) {
    // cmd parser
    ArgParser parser(argc, (const char**)argv);
    std::string xclbin_path;
    if (!parser.getCmdOption("-xclbin", xclbin_path)) {
        std::cout << "ERROR:xclbin path is not set!\n";
        return 1;
    }

    // set repeat time
    int num_rep = 2;
    std::string num_str;
    if (parser.getCmdOption("-rep", num_str)) {
        try {
            num_rep = std::stoi(num_str);
        } catch (...) {
            num_rep = 2;
        }
    }
    if (num_rep < 2) {
        num_rep = 2;
        std::cout << "WARNING: ping-pong buffer shoulde be updated at least 1 time.\n";
    }
    if (num_rep > 20) {
        num_rep = 20;
        std::cout << "WARNING: limited repeat to " << num_rep << " times.\n";
    }

    // input data
    const char datain[] = {0x01};

    // cipher key
    const unsigned char key[] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c,
                                 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29,
                                 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
                                 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43,
                                 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f};

    // generate golden
    // ouput length of the result
    int outlen = 0;
    // output result
    unsigned char golden[N_ROW];

    // call OpenSSL API to get the golden
    EVP_CIPHER_CTX* ctx;
    ctx = EVP_CIPHER_CTX_new();
    EVP_CipherInit_ex(ctx, EVP_rc4(), NULL, NULL, NULL, 1);
    EVP_CIPHER_CTX_set_key_length(ctx, KEY_SIZE);
    EVP_CipherInit_ex(ctx, NULL, NULL, key, NULL, 1);
    for (unsigned int i = 0; i < N_ROW; i++) {
        EVP_CipherUpdate(ctx, golden + i, &outlen, (const unsigned char*)datain, 1);
    }
    EVP_CIPHER_CTX_free(ctx);

    ap_uint<512> keyBlock[4];
    for (unsigned int i = 0; i < KEY_SIZE; i++) {
        keyBlock[i / 64].range((i % 64) * 8 + 7, (i % 64) * 8) = key[i];
    }

    ap_uint<512> dataBlock;
    for (unsigned int i = 0; i < 64; i++) {
        dataBlock.range(i * 8 + 7, i * 8) = datain[0];
    }

    std::cout << "Goldens have been created using OpenSSL.\n";

    // Host buffers
    ap_uint<512>* hb_in1 = aligned_alloc<ap_uint<512> >(MAX_ROW);
    ap_uint<512>* hb_in2 = aligned_alloc<ap_uint<512> >(MAX_ROW);
    ap_uint<512>* hb_in3 = aligned_alloc<ap_uint<512> >(MAX_ROW);
    ap_uint<512>* hb_in4 = aligned_alloc<ap_uint<512> >(MAX_ROW);
    ap_uint<512>* hb_out_a[4];
    for (int i = 0; i < 4; i++) {
        hb_out_a[i] = aligned_alloc<ap_uint<512> >(MAX_ROW);
    }
    ap_uint<512>* hb_out_b[4];
    for (int i = 0; i < 4; i++) {
        hb_out_b[i] = aligned_alloc<ap_uint<512> >(MAX_ROW);
    }

    // generate configuration block
    hb_in1[0].range(127, 0) = N_ROW;
    hb_in1[0].range(191, 128) = N_TASK;
    hb_in1[0].range(207, 192) = KEY_SIZE;
    hb_in2[0].range(127, 0) = N_ROW;
    hb_in2[0].range(191, 128) = N_TASK;
    hb_in2[0].range(207, 192) = KEY_SIZE;
    hb_in3[0].range(127, 0) = N_ROW;
    hb_in3[0].range(191, 128) = N_TASK;
    hb_in3[0].range(207, 192) = KEY_SIZE;
    hb_in4[0].range(127, 0) = N_ROW;
    hb_in4[0].range(191, 128) = N_TASK;
    hb_in4[0].range(207, 192) = KEY_SIZE;
    // generate key blocks
    for (unsigned int j = 0; j < CH_NM * 4; j++) {
        hb_in1[j + 1] = keyBlock[j % 4];
        hb_in2[j + 1] = keyBlock[j % 4];
        hb_in3[j + 1] = keyBlock[j % 4];
        hb_in4[j + 1] = keyBlock[j % 4];
    }
    // generate texts
    for (unsigned int j = 0; j < N_ROW * N_TASK * CH_NM / 64; j++) {
        hb_in1[j + 1 + 4 * CH_NM] = dataBlock;
        hb_in2[j + 1 + 4 * CH_NM] = dataBlock;
        hb_in3[j + 1 + 4 * CH_NM] = dataBlock;
        hb_in4[j + 1 + 4 * CH_NM] = dataBlock;
    }

    std::cout << "Host map buffer has been allocated and set.\n";

    // Get CL devices.
    std::vector<cl::Device> devices = xcl::get_xil_devices();
    cl::Device device = devices[0];

    // Create context and command queue for selected device
    cl::Context context(device);
    cl::CommandQueue q(context, device, CL_QUEUE_PROFILING_ENABLE | CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE);
    std::string devName = device.getInfo<CL_DEVICE_NAME>();
    std::cout << "Selected Device " << devName << "\n";

    cl::Program::Binaries xclBins = xcl::import_binary_file(xclbin_path);
    devices.resize(1);
    cl::Program program(context, devices, xclBins);

    cl::Kernel kernel0(program, "rc4EncryptKernel_1");
    cl::Kernel kernel1(program, "rc4EncryptKernel_2");
    cl::Kernel kernel2(program, "rc4EncryptKernel_3");
    cl::Kernel kernel3(program, "rc4EncryptKernel_4");
    std::cout << "Kernel has been created.\n";

    cl_mem_ext_ptr_t mext_in[4];
    mext_in[0] = {XCL_MEM_DDR_BANK0, hb_in1, 0};
    mext_in[1] = {XCL_MEM_DDR_BANK1, hb_in2, 0};
    mext_in[2] = {XCL_MEM_DDR_BANK2, hb_in3, 0};
    mext_in[3] = {XCL_MEM_DDR_BANK3, hb_in4, 0};

    cl_mem_ext_ptr_t mext_out_a[4];
    mext_out_a[0] = {XCL_MEM_DDR_BANK0, hb_out_a[0], 0};
    mext_out_a[1] = {XCL_MEM_DDR_BANK1, hb_out_a[1], 0};
    mext_out_a[2] = {XCL_MEM_DDR_BANK2, hb_out_a[2], 0};
    mext_out_a[3] = {XCL_MEM_DDR_BANK3, hb_out_a[3], 0};

    cl_mem_ext_ptr_t mext_out_b[4];
    mext_out_b[0] = {XCL_MEM_DDR_BANK0, hb_out_b[0], 0};
    mext_out_b[1] = {XCL_MEM_DDR_BANK1, hb_out_b[1], 0};
    mext_out_b[2] = {XCL_MEM_DDR_BANK2, hb_out_b[2], 0};
    mext_out_b[3] = {XCL_MEM_DDR_BANK3, hb_out_b[3], 0};

    // ping buffer
    cl::Buffer in_buff_a[4];
    cl::Buffer out_buff_a[4];
    // pong buffer
    cl::Buffer in_buff_b[4];
    cl::Buffer out_buff_b[4];

    // Map buffers
    for (int i = 0; i < 4; i++) {
        in_buff_a[i] = cl::Buffer(context, CL_MEM_EXT_PTR_XILINX | CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY,
                                  (size_t)(sizeof(ap_uint<512>) * MAX_ROW), &mext_in[i]);
        out_buff_a[i] = cl::Buffer(context, CL_MEM_EXT_PTR_XILINX | CL_MEM_USE_HOST_PTR | CL_MEM_WRITE_ONLY,
                                   (size_t)(sizeof(ap_uint<512>) * MAX_ROW), &mext_out_a[i]);
        in_buff_b[i] = cl::Buffer(context, CL_MEM_EXT_PTR_XILINX | CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY,
                                  (size_t)(sizeof(ap_uint<512>) * MAX_ROW), &mext_in[i]);
        out_buff_b[i] = cl::Buffer(context, CL_MEM_EXT_PTR_XILINX | CL_MEM_USE_HOST_PTR | CL_MEM_WRITE_ONLY,
                                   (size_t)(sizeof(ap_uint<512>) * MAX_ROW), &mext_out_b[i]);
    }

    std::cout << "DDR buffers have been mapped/copy-and-mapped\n";

    q.finish();

    struct timeval start_time, end_time;
    gettimeofday(&start_time, 0);

    std::vector<std::vector<cl::Event> > write_events(num_rep);
    std::vector<std::vector<cl::Event> > kernel_events(num_rep);
    std::vector<std::vector<cl::Event> > read_events(num_rep);
    for (int i = 0; i < num_rep; i++) {
        write_events[i].resize(1);
        kernel_events[i].resize(4);
        read_events[i].resize(1);
    }

    /*
     * W0-. W1----.     W2-.     W3-.
     *    '-K0--. '-K1-/-. '-K2-/-. '-K3---.
     *          '---R0-  '---R1-  '---R2   '--R3
     */
    for (int i = 0; i < num_rep; i++) {
        int use_a = i & 1;

        // write data to DDR
        std::vector<cl::Memory> ib;
        if (use_a) {
            ib.push_back(in_buff_a[0]);
            ib.push_back(in_buff_a[1]);
            ib.push_back(in_buff_a[2]);
            ib.push_back(in_buff_a[3]);
        } else {
            ib.push_back(in_buff_b[0]);
            ib.push_back(in_buff_b[1]);
            ib.push_back(in_buff_b[2]);
            ib.push_back(in_buff_b[3]);
        }

        if (i > 1) {
            q.enqueueMigrateMemObjects(ib, 0, &read_events[i - 2], &write_events[i][0]);
        } else {
            q.enqueueMigrateMemObjects(ib, 0, nullptr, &write_events[i][0]);
        }

        // set args and enqueue kernel
        if (use_a) {
            int j = 0;
            kernel0.setArg(j++, in_buff_a[0]);
            kernel0.setArg(j++, out_buff_a[0]);
            j = 0;
            kernel1.setArg(j++, in_buff_a[1]);
            kernel1.setArg(j++, out_buff_a[1]);
            j = 0;
            kernel2.setArg(j++, in_buff_a[2]);
            kernel2.setArg(j++, out_buff_a[2]);
            j = 0;
            kernel3.setArg(j++, in_buff_a[3]);
            kernel3.setArg(j++, out_buff_a[3]);
        } else {
            int j = 0;
            kernel0.setArg(j++, in_buff_b[0]);
            kernel0.setArg(j++, out_buff_b[0]);
            j = 0;
            kernel1.setArg(j++, in_buff_b[1]);
            kernel1.setArg(j++, out_buff_b[1]);
            j = 0;
            kernel2.setArg(j++, in_buff_b[2]);
            kernel2.setArg(j++, out_buff_b[2]);
            j = 0;
            kernel3.setArg(j++, in_buff_b[3]);
            kernel3.setArg(j++, out_buff_b[3]);
        }
        q.enqueueTask(kernel0, &write_events[i], &kernel_events[i][0]);
        q.enqueueTask(kernel1, &write_events[i], &kernel_events[i][1]);
        q.enqueueTask(kernel2, &write_events[i], &kernel_events[i][2]);
        q.enqueueTask(kernel3, &write_events[i], &kernel_events[i][3]);

        // read data from DDR
        std::vector<cl::Memory> ob;
        if (use_a) {
            ob.push_back(out_buff_a[0]);
            ob.push_back(out_buff_a[1]);
            ob.push_back(out_buff_a[2]);
            ob.push_back(out_buff_a[3]);
        } else {
            ob.push_back(out_buff_b[0]);
            ob.push_back(out_buff_b[1]);
            ob.push_back(out_buff_b[2]);
            ob.push_back(out_buff_b[3]);
        }
        q.enqueueMigrateMemObjects(ob, CL_MIGRATE_MEM_OBJECT_HOST, &kernel_events[i], &read_events[i][0]);
    }

    // wait all to finish
    q.flush();
    q.finish();
    gettimeofday(&end_time, 0);
    std::cout << "Kernel has been run for " << std::dec << num_rep << " times." << std::endl;
    std::cout << "Execution time " << tvdiff(&start_time, &end_time) << "us" << std::endl;

    // check result
    bool checked = true;
    // check ping buffer
    for (unsigned int n = 0; n < 4; n++) {
        for (unsigned int j = 0; j < N_TASK; j++) {
            for (unsigned int i = 0; i < N_ROW; i++) {
                for (unsigned int k = 0; k < CH_NM; k++) {
                    if (hb_out_a[n][i / (64 / CH_NM) + j * (N_ROW / (64 / CH_NM))].range(
                            8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM + 7,
                            8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM) != golden[i]) {
                        checked = false;
                        std::cout << "Error found in kernel_1 " << std::dec << k << " channel, " << j << " task, " << i
                                  << " message" << std::endl;
                        std::cout << "golden = " << std::hex << golden[i] << std::endl;
                        std::cout << "fpga   = " << std::hex
                                  << hb_out_a[n][i / (64 / CH_NM) + j * (N_ROW / (64 / CH_NM))].range(
                                         8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM + 7,
                                         8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM)
                                  << std::endl;
                    }

                    if (hb_out_a[n][i / (64 / CH_NM) + j * (N_ROW / (64 / CH_NM))].range(
                            8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM + 7,
                            8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM) != golden[i]) {
                        checked = false;
                        std::cout << "Error found in kernel_2" << std::dec << k << " channel, " << j << " task, " << i
                                  << " message" << std::endl;
                        std::cout << "golden = " << std::hex << golden[i] << std::endl;
                        std::cout << "fpga   = " << std::hex
                                  << hb_out_a[n][i / (64 / CH_NM) + j * (N_ROW / (64 / CH_NM))].range(
                                         8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM + 7,
                                         8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM)
                                  << std::endl;
                    }

                    if (hb_out_a[n][i / (64 / CH_NM) + j * (N_ROW / (64 / CH_NM))].range(
                            8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM + 7,
                            8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM) != golden[i]) {
                        checked = false;
                        std::cout << "Error found in kernel_3" << std::dec << k << " channel, " << j << " task, " << i
                                  << " message" << std::endl;
                        std::cout << "golden = " << std::hex << golden[i] << std::endl;
                        std::cout << "fpga   = " << std::hex
                                  << hb_out_a[n][i / (64 / CH_NM) + j * (N_ROW / (64 / CH_NM))].range(
                                         8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM + 7,
                                         8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM)
                                  << std::endl;
                    }

                    if (hb_out_a[n][i / (64 / CH_NM) + j * (N_ROW / (64 / CH_NM))].range(
                            8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM + 7,
                            8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM) != golden[i]) {
                        checked = false;
                        std::cout << "Error found in kernel_4" << std::dec << k << " channel, " << j << " task, " << i
                                  << " message" << std::endl;
                        std::cout << "golden = " << std::hex << golden[i] << std::endl;
                        std::cout << "fpga   = " << std::hex
                                  << hb_out_a[n][i / (64 / CH_NM) + j * (N_ROW / (64 / CH_NM))].range(
                                         8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM + 7,
                                         8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM)
                                  << std::endl;
                    }
                }
            }
        }
    }

    // check pong buffer
    for (unsigned int n = 0; n < 4; n++) {
        for (unsigned int j = 0; j < N_TASK; j++) {
            for (unsigned int i = 0; i < N_ROW; i++) {
                for (unsigned int k = 0; k < CH_NM; k++) {
                    if (hb_out_b[n][i / (64 / CH_NM) + j * (N_ROW / (64 / CH_NM))].range(
                            8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM + 7,
                            8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM) != golden[i]) {
                        checked = false;
                        std::cout << "Error found in kernel_1 " << std::dec << k << " channel, " << j << " task, " << i
                                  << " message" << std::endl;
                        std::cout << "golden = " << std::hex << golden[i] << std::endl;
                        std::cout << "fpga   = " << std::hex
                                  << hb_out_b[n][i / (64 / CH_NM) + j * (N_ROW / (64 / CH_NM))].range(
                                         8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM + 7,
                                         8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM)
                                  << std::endl;
                    }

                    if (hb_out_b[n][i / (64 / CH_NM) + j * (N_ROW / (64 / CH_NM))].range(
                            8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM + 7,
                            8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM) != golden[i]) {
                        checked = false;
                        std::cout << "Error found in kernel_2" << std::dec << k << " channel, " << j << " task, " << i
                                  << " message" << std::endl;
                        std::cout << "golden = " << std::hex << golden[i] << std::endl;
                        std::cout << "fpga   = " << std::hex
                                  << hb_out_b[n][i / (64 / CH_NM) + j * (N_ROW / (64 / CH_NM))].range(
                                         8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM + 7,
                                         8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM)
                                  << std::endl;
                    }

                    if (hb_out_b[n][i / (64 / CH_NM) + j * (N_ROW / (64 / CH_NM))].range(
                            8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM + 7,
                            8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM) != golden[i]) {
                        checked = false;
                        std::cout << "Error found in kernel_3" << std::dec << k << " channel, " << j << " task, " << i
                                  << " message" << std::endl;
                        std::cout << "golden = " << std::hex << golden[i] << std::endl;
                        std::cout << "fpga   = " << std::hex
                                  << hb_out_b[n][i / (64 / CH_NM) + j * (N_ROW / (64 / CH_NM))].range(
                                         8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM + 7,
                                         8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM)
                                  << std::endl;
                    }

                    if (hb_out_b[n][i / (64 / CH_NM) + j * (N_ROW / (64 / CH_NM))].range(
                            8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM + 7,
                            8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM) != golden[i]) {
                        checked = false;
                        std::cout << "Error found in kernel_4" << std::dec << k << " channel, " << j << " task, " << i
                                  << " message" << std::endl;
                        std::cout << "golden = " << std::hex << golden[i] << std::endl;
                        std::cout << "fpga   = " << std::hex
                                  << hb_out_b[n][i / (64 / CH_NM) + j * (N_ROW / (64 / CH_NM))].range(
                                         8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM + 7,
                                         8 * (i % (64 / CH_NM)) + 8 * k * 64 / CH_NM)
                                  << std::endl;
                    }
                }
            }
        }
    }

    if (checked) {
        std::cout << std::dec << CH_NM << " channels, " << N_TASK << " tasks, " << N_ROW
                  << " messages verified. No error found!" << std::endl;
    }

    return 0;
}
