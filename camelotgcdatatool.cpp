#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <stdio.h>
#include <stdint.h>
#include <string>
#include <vector>

struct CompressState {
    std::vector<uint8_t> *out_buf;
    uint8_t flags = 0;
    uint8_t num_codebits = 0;
    uint8_t code_pos = 0;
    uint8_t code_buf[24];
};

uint8_t ReadFileU8(FILE *file)
{
    uint8_t temp;
    if (fread(&temp, 1, 1, file) != 1) {
        std::cout << "Failed to read file." << std::endl;
        exit(1);
    }
    return temp;
}

uint32_t ReadFileU32(FILE *file)
{
    uint8_t temp[4];
    if (fread(&temp, 4, 1, file) != 1) {
        std::cout << "Failed to read file." << std::endl;
        exit(1);
    }
    return (temp[0] << 24) | (temp[1] << 16) | (temp[2] << 8) | temp[3];
}

std::vector<uint8_t> ReadFileWhole(FILE *file)
{
    uint32_t old_ofs = ftell(file);
    uint32_t size;
    std::vector<uint8_t> data;
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    data.resize(size);
    fseek(file, 0, SEEK_SET);
    fread(&data[0], size, 1, file);
    fseek(file, old_ofs, SEEK_SET);
    return data;
}

void WriteFileU8(FILE *file, uint8_t value)
{
    fwrite(&value, 1, 1, file);
}

void WriteFileU32(FILE *file, uint32_t value)
{
    uint8_t temp[4];
    temp[0] = value >> 24;
    temp[1] = (value >> 16) & 0xFF;
    temp[2] = (value >> 8) & 0xFF;
    temp[3] = value & 0xFF;
    fwrite(temp, 4, 1, file);
}

void PadFile(FILE *file, uint32_t to)
{
    uint8_t zero = 0;
    while ((ftell(file) % to) != 0) {
        fwrite(&zero, 1, 1, file);
    }
}

bool DecodeData(std::string in_name, std::string out_name)
{
    FILE *in_file = fopen(in_name.c_str(), "rb");
    FILE *out_file;
    if (!in_file) {
        std::cout << "Failed to open " << in_name << " for reading." << std::endl;
        return false;
    }
    uint32_t header = ReadFileU32(in_file);
    uint32_t type = header >> 24;
    uint32_t out_size = header & 0xFFFFFF;
    uint32_t out_pos = 0;
    uint8_t *out = new uint8_t[out_size];
    
    if (type != 1 && type != 2) {
        std::cout << "Invalid input file." << std::endl;
        return false;
    }
    while (1) {
        uint32_t flag = ReadFileU8(in_file) | 0x100;
        while (flag < 0x10000) {
            if (flag & 0x80) {
                uint32_t len = ReadFileU8(in_file);
                uint32_t back_ptr = (len << 4) & 0xF00;
                back_ptr |= ReadFileU8(in_file);
                len &= 0xF;
                if (back_ptr == 0) {
                    goto end_decompress;
                }
                if (len == 0) {
                    len = ReadFileU8(in_file) + 16;
                }
                for (uint32_t i = 0; i < len + 1; i++) {
                    out[out_pos] = out[out_pos-back_ptr];
                    out_pos++;
                }
            } else {
                out[out_pos] = ReadFileU8(in_file);
                out_pos++;
            }
            flag <<= 1;
        }
    }
end_decompress:
    out_file = fopen(out_name.c_str(), "wb");
    if (!out_file) {
        std::cout << "Failed to open " << out_name << " for writing." << std::endl;
        delete[] out;
        fclose(in_file);
        return false;
    }
    fwrite(out, out_pos, 1, out_file);
    fclose(out_file);
    if (type == 2) {
        std::string reloc_name = out_name + ".rel";
        FILE *reloc_file = fopen(reloc_name.c_str(), "wb");
        if (!reloc_file) {
            std::cout << "Failed to open " << reloc_name << " for writing." << std::endl;
            delete[] out;
            fclose(in_file);
            return false;
        }
        uint32_t file_ofs = (ftell(in_file) + 7) & 0xFFFFFFF8;
        fseek(in_file, file_ofs, SEEK_SET);
        while (1) {
            uint32_t word1 = ReadFileU32(in_file);
            uint32_t word2 = ReadFileU32(in_file);
            if ((word1 >> 24) == 0xFF) {
                WriteFileU32(reloc_file, word1);
                WriteFileU32(reloc_file, word2);
                break;
            }
            WriteFileU32(reloc_file, word1);
            WriteFileU32(reloc_file, word2);
        }
        fclose(reloc_file);
    }
    delete[] out;
    fclose(in_file);
    return true;
}

void CompressOutputCodeBuf(CompressState &state)
{
    state.out_buf->push_back(state.flags);
    for (uint8_t i = 0; i < state.code_pos; i++) {
        state.out_buf->push_back(state.code_buf[i]);
    }
    state.num_codebits = 0;
    state.flags = 0;
    state.code_pos = 0;
}

void CompressPushBackRef(CompressState &state, uint16_t back_ofs, uint16_t size)
{
    state.flags |= (1 << (7 - state.num_codebits));
    if (size >= 17) {
        state.code_buf[state.code_pos++] = (back_ofs & 0xF00) >> 4;
        state.code_buf[state.code_pos++] = back_ofs & 0xFF;
        state.code_buf[state.code_pos++] = (size - 17);
    } else {
        state.code_buf[state.code_pos++] = ((back_ofs & 0xF00) >> 4) | ((size - 1) & 0xF);
        state.code_buf[state.code_pos++] = back_ofs & 0xFF;
    }
    state.num_codebits++;
    if (state.num_codebits == 8) {
        CompressOutputCodeBuf(state);
    }
}

void CompressPushLiteral(CompressState &state, uint8_t value)
{
    state.code_buf[state.code_pos++] = value;
    state.num_codebits++;
    if (state.num_codebits == 8) {
        CompressOutputCodeBuf(state);
    }
}

void CompressPushTerminator(CompressState &state)
{
    CompressPushBackRef(state, 0, 2);
    if (state.num_codebits != 0) {
        CompressOutputCodeBuf(state);
    }
}

// simple and straight encoding scheme for Yaz0
uint32_t simpleEnc(std::vector<uint8_t> &src, int pos, size_t *pMatchPos)
{
    size_t startPos = (pos < 0xFFF) ? 0 : (pos - 0xFFF);
    size_t numBytes = 1;
    size_t matchPos = 0;

    for (size_t i = startPos; i < pos; i++)
    {
        size_t j;
        for (j = 0; j < src.size() - pos; j++)
        {
            if (src[i + j] != src[j + pos])
                break;
        }
        if (j > numBytes)
        {
            numBytes = j;
            matchPos = i;
        }
    }
    *pMatchPos = matchPos;
    if (numBytes == 2)
        numBytes = 1;
    return numBytes;
}

// a lookahead encoding scheme for ngc Yaz0
size_t nintendoEnc(std::vector<uint8_t> &src, size_t pos, size_t *pMatchPos)
{
    size_t numBytes = 1;
    static size_t numBytes1;
    static size_t matchPos;
    static int prevFlag = 0;

    // if prevFlag is set, it means that the previous position was determined by look-ahead try.
    // so just use it. this is not the best optimization, but nintendo's choice for speed.
    if (prevFlag == 1) {
        *pMatchPos = matchPos;
        prevFlag = 0;
        return numBytes1;
    }
    prevFlag = 0;
    numBytes = simpleEnc(src, pos, &matchPos);
    *pMatchPos = matchPos;

    // if this position is RLE encoded, then compare to copying 1 byte and next position(pos+1) encoding
    if (numBytes >= 3) {
        numBytes1 = simpleEnc(src, pos + 1, &matchPos);
        // if the next position encoding is +2 longer than current position, choose it.
        // this does not guarantee the best optimization, but fairly good optimization with speed.
        if (numBytes1 >= numBytes + 2) {
            numBytes = 1;
            prevFlag = 1;
        }
    }
    return numBytes;
}


std::vector<uint8_t> CompressBuffer(std::vector<uint8_t> &in_buf)
{
    CompressState state;
    uint32_t effective_size = in_buf.size() & 0xFFFFFF;
    std::vector<uint8_t> out_data;
    size_t src_pos = 0;
    state.out_buf = &out_data;
    out_data.push_back(0x1);
    out_data.push_back((effective_size & 0xFF0000) >> 16);
    out_data.push_back((effective_size & 0xFF00) >> 8);
    out_data.push_back(effective_size & 0xFF);
    while(src_pos < effective_size) {
        size_t match_pos;
        size_t num_bytes = nintendoEnc(in_buf, src_pos, &match_pos);
        if (num_bytes < 3) {
            CompressPushLiteral(state, in_buf[src_pos++]);
        } else {
            if (num_bytes >= 272) {
                num_bytes = 272;
            }
            CompressPushBackRef(state, src_pos-match_pos, num_bytes);
            src_pos += num_bytes;
        }
    }
    CompressPushTerminator(state);
    return out_data;
}


bool EncodeData(std::string in_name, std::string out_name)
{
    FILE *in_file = fopen(in_name.c_str(), "rb");
    FILE *out_file;
    if (!in_file) {
        std::cout << "Failed to open " << in_name << " for reading." << std::endl;
        return false;
    }
    std::vector<uint8_t> in_buf = ReadFileWhole(in_file);
    fclose(in_file);
    out_file = fopen(out_name.c_str(), "wb");
    if (!out_file) {
        std::cout << "Failed to open " << out_name << " for writing." << std::endl;
        return false;
    }
    std::vector<uint8_t> out_buf = CompressBuffer(in_buf);
    out_file = fopen(out_name.c_str(), "wb");
    if (!out_file) {
        std::cout << "Failed to open " << out_name << " for writing." << std::endl;
        return false;
    }
    std::string reloc_name = in_name+".rel";
    FILE *reloc_file = fopen(reloc_name.c_str(), "rb");
    if (reloc_file) {
        out_buf[0] = 2;
        fwrite(&out_buf[0], out_buf.size(), 1, out_file);
        PadFile(out_file, 8);
        std::vector<uint8_t> reloc_buf = ReadFileWhole(reloc_file);
        fwrite(&reloc_buf[0], reloc_buf.size(), 1, out_file);
        fclose(reloc_file);
    } else {
        fwrite(&out_buf[0], out_buf.size(), 1, out_file);
        PadFile(out_file, 4);
    }
    fclose(out_file);
    return true;
}

int main(int argc, char **argv)
{
    std::string option;
    std::string in_name;
    std::string out_name;
    if (argc != 3 && argc != 4) {
        std::cout << "Usage: " << argv[0] << " d|e in [out]" << std::endl;
        std::cout << "d for the second argument represents decoding a file" << std::endl;
        std::cout << "e for the second argument represents encoding a file" << std::endl;
        return 1;
    }
    option = argv[1];
    in_name = argv[2];
    if (argc == 4) {
        out_name = argv[3];
    }
    if (option == "d") {
        if (argc != 4) {
            out_name = in_name + ".bin";
        }
        return !DecodeData(in_name, out_name);
    } else if (option == "e") {
        if (argc != 4) {
            out_name = in_name.substr(0, in_name.find_last_of('.'));
        }
        return !EncodeData(in_name, out_name);
    } else {
        std::cout << "Invalid option " << option << std::endl;
    }
    return 0;
}