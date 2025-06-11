import matplotlib
import os
import sys
import traceback

# Force flush output
print("Script started", flush=True)
try:
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    print("Backend:", matplotlib.get_backend(), flush=True)
    print("Python version:", sys.version, flush=True)
    print("Matplotlib version:", matplotlib.__version__, flush=True)
    print("Current working directory:", os.getcwd(), flush=True)

    # Test file write
    test_file = os.path.abspath('test.txt')
    with open(test_file, 'w') as f:
        f.write("Test")
    print(f"Wrote {test_file}", flush=True)

    # Plot and save
    plt.plot([1, 2, 3], [4, 5, 7])
    plt.title("Test Plot")
    output_path = os.path.abspath('test_plot.png')
    plt.savefig(output_path)
    plt.close()
    print(f"Plot saved to: {output_path}", flush=True)
    print("File exists:", os.path.exists(output_path), flush=True)
except Exception as e:
    print(f"Error: {e}", flush=True)
    traceback.print_exc(file=sys.stdout)
print("Script ended", flush=True)