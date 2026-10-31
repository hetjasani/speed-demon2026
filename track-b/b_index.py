import os

def main(data_dir, out_dir):
    binary = "/app/b_index_cpp"
    os.execv(binary, [binary, data_dir, out_dir])
