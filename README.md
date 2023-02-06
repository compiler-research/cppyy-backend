# Setup cppyy for development

## Installing the packages

### Install cling

Get the cpt.py script from cling repository and use it to create a cling 
development environment:

```
wget https://raw.githubusercontent.com/root-project/cling/master/tools/packaging/cpt.py
chmod +x cpt.py
cpt.py --create-dev-env Debug --with-workdir=./cling-build/
```

Note the directory in which you run the `cpt.py` script as it will later be
required multiple times. This will be referred to as `CPT_DIR`:

```
export CPT_DIR=$PWD
cd ..
```

### Install InterOp

Clone the InterOp repo. Build it using cling and install:

```
git clone https://github.com/compiler-research/InterOp.git
cd InterOp
mkdir build install && cd build
cmake -DUSE_CLING=ON -DCling_DIR=$CPT_DIR/src/cling/builddir -DCMAKE_INSTALL_PREFIX=$PWD/../install ..
cmake --build . --target install
```

Note down the path to InterOp install directory. This will be referred to as
`INTEROP_DIR`:

```
cd ../install
export INTEROP_DIR=$PWD
cd ../..
```

### Install cppyy-backend

Clone the repo and edit the `src/clingwrapper.cxx` file to change the `CPT_DIR`
and `INTEROP_DIR` variables at line 190:

```
git clone https://github.com/compiler-research/cppyy-backend.git
cd cppyy-backend
echo "std::string CPT_DIR = \"$CPT_DIR\" \nstd::string INTEROP_DIR = \"$INTEROP_DIR\""
edit src/clingwrapper.cxx
```

Build the repo and copy library files into `python/cppyy-backend` directory:
```
mkdir python/cppyy_backend/lib build && cd build
cmake -DInterOp_DIR=$INTEROP_DIR ..
cmake --build .
cp libcppyy-backend.so ../python/cppyy_backend/lib/
cp $CPT_DIR/src/cling/builddir/lib/libcling.so ../python/cppyy_backend/lib/
```

Note down the path to `cppyy-backend/python` directory as "CB_PYTHON_DIR":
```
cd ../python
export CB_PYTHON_DIR=$PWD
cd ../..
```

### Install CPyCppyy

Clone the repo, checkout `rm-root-meta` branch and build it:
```
git clone https://github.com/sudo-panda/CPyCppyy.git
cd CPyCppyy
git checkout rm-root-meta
mkdir build && cd build
cmake ..
cmake --build .
```

Note down the path to the `build` directory as `CPYCPPYY_DIR`:
```
export CPYCPPYY_DIR=$PWD
cd ../..
```

### Install cppyy

Create virtual environment and activate it:
```
python3 -m venv .venv
source .venv/bin/activate
```

Clone the repo, checkout `rm-root-meta` branch and install:
```
git clone https://github.com/sudo-panda/cppyy.git
cd cppyy
git checkout rm-root-meta
python -m pip install --upgrade . --no-deps
cd ..
```

## Run cppyy

Each time you want to run cppyy you need to:
1. Activate the virtual environment
    ```
    source .venv/bin/activate
    ```
2. Add `CPYCPPYY_DIR` and `CB_PYTHON_DIR` to `PYTHONPATH`:
    ```
    export PYTHONPATH=$PYTHONPATH:$CPYCPPYY_DIR:$CB_PYTHON_DIR
    ```
    The `CPYCPPYY_DIR` and `CB_PYTHON_DIR` will not be set if you start a new
    terminal instance so you can replace them with the actual path that they
    refer to.

Now you can `import cppyy` in `python`
```
python -c "import cppyy"
```

## Run the tests

**Follow the steps in Run cppyy.** Change to the test directory, make the library files and run pytest:
```
cd cppyy/test
make all
python -m pytest -sv
```
