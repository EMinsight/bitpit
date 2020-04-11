/*---------------------------------------------------------------------------*\
 *
 *  bitpit
 *
 *  Copyright (C) 2015-2019 OPTIMAD engineering Srl
 *
 *  -------------------------------------------------------------------------
 *  License
 *  This file is part of bitpit.
 *
 *  bitpit is free software: you can redistribute it and/or modify it
 *  under the terms of the GNU Lesser General Public License v3 (LGPL)
 *  as published by the Free Software Foundation.
 *
 *  bitpit is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
 *  License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with bitpit. If not, see <http://www.gnu.org/licenses/>.
 *
\*---------------------------------------------------------------------------*/

#include <stdexcept>
#include <string>
#include <unordered_set>

#include "bitpit_IO.hpp"

#include "system_solvers_large.hpp"

namespace bitpit {

int SystemSolver::m_nInstances = 0;
bool SystemSolver::m_optionsEditable = true;
std::vector<std::string> SystemSolver::m_options = std::vector<std::string>(1, "bitpit");

/*!
 * Add an initialization option
 *
 * \param option is the option that will be added
 */
void SystemSolver::addInitOption(const std::string &option)
{
    if (!m_optionsEditable) {
        throw std::runtime_error("Initialization opions can be set only before initializing the solver.");
    }

    m_options.push_back(option);
}

/*!
 * Add initialization options
 *
 * \param argc is a non-negative value representing the number of arguments
 * passed to the program from the environment in which the program is run
 * \param argv is a pointer to the first element of an array of argc + 1
 * pointers, of which the last one is null and the previous ones, if any,
 * point to null-terminated multibyte strings that represent the arguments
 * passed to the program from the execution environment. If argv[0] is not
 * a null pointer (or, equivalently, if argc > 0), it points to a string
 * that represents the name used to invoke the program, or to an empty string.
 * The value of argv[0] is not propagated to the system solver, the solver
 * will see a dummy name.
 */
void SystemSolver::addInitOptions(int argc, char **argv)
{
    if (!m_optionsEditable) {
        throw std::runtime_error("Initialization opions can be set only before initializing the solver.");
    }

    for (int i = 1; i < argc; ++i) {
        m_options.push_back(argv[i]);
    }
}

/*!
 * Add initialization options
 *
 * \param options are the options that will be added
 */
void SystemSolver::addInitOptions(const std::vector<std::string> &options)
{
    if (!m_optionsEditable) {
        throw std::runtime_error("Initialization opions can be set only before initializing the solver.");
    }

    for (const std::string &option : options) {
        m_options.push_back(option);
    }
}

/*!
 * Clear initialization options
 */
void SystemSolver::clearInitOptions()
{
    m_options.clear();
}

/*!
 * Constuctor
 *
 * \param debug if set to true, debug information will be printed
 */
SystemSolver::SystemSolver(bool debug)
    : SystemSolver("", debug)
{
}

/*!
 * Constuctor
 *
 * \param prefix is the prefix string to prepend to all option requests
 * \param debug if set to true, debug information will be printed
 */
SystemSolver::SystemSolver(const std::string &prefix, bool debug)
    : m_A(nullptr), m_rhs(nullptr), m_solution(nullptr),
      m_KSP(nullptr),
      m_prefix(prefix), m_assembled(false), m_setUp(false),
#if BITPIT_ENABLE_MPI==1
      m_communicator(MPI_COMM_SELF), m_partitioned(false),
      m_rowGlobalOffset(0), m_colGlobalOffset(0),
#endif
      m_rowPermutation(nullptr), m_colPermutation(nullptr)
{
    // Add debug options
    if (debug) {
        addInitOption("-log_view");
        addInitOption("-ksp_monitor_true_residual");
        addInitOption("-ksp_converged_reason");
        addInitOption("-ksp_monitor_singular_value");
    }

    // Initialize Petsc
    if (m_nInstances == 0) {
        // Generate command line arguments
        //
        // The first argument is the executable name and it is set to a
        // dummy value.
        std::string help        = "None";
        std::string programName = "bitpit_system_solver";

        int argc = 1 + m_options.size();
        char **argv = new char*[argc + 1];
        argv[0] = strdup(programName.data());
        for (std::size_t i = 0; i < m_options.size(); i++) {
            argv[1 + i] = strdup(m_options[i].data());
        }
        argv[argc] = nullptr;

        // Call initialization
        PetscInitialize(&argc, &argv, 0, help.data());

        // Clean-up
        for (int i = 0; i < argc; ++i) {
            free(argv[i]);
        }

        delete[] argv;
    }

    // Increase the number of instances
    ++m_nInstances;
}

/*!
 * Destructor
 */
SystemSolver::~SystemSolver()
{
    // Clear the solver
    clear();

    // Reset the permutations
    resetPermutations();

    // Decrease the number of instances
    --m_nInstances;

    // Finalize petsc
    if (m_nInstances == 0) {
        PetscFinalize();
    }
}

/*!
 * Clear the system
 */
void SystemSolver::clear()
{
    if (isSetUp()) {
        KSPDestroy(&m_KSP);
        m_KSP = nullptr;

        m_setUp = false;
    }

    if (!isAssembled()) {
        MatDestroy(&m_A);
        VecDestroy(&m_rhs);
        VecDestroy(&m_solution);

#if BITPIT_ENABLE_MPI==1
        freeCommunicator();
#endif

        m_assembled = false;
    }

    if (m_nInstances == 0) {
        m_optionsEditable = true;
    }
}

/*!
 * Set the permutations that will use internally by the solver.
 *
 * Only local permutations are suppoerted.
 *
 * \param nRows are the rows of the matrix
 * \param rowRanks are the rank of the rows
 * \param nRows are the columns of the matrix
 * \param colRanks are the rank of the columns
 */
void SystemSolver::setPermutations(long nRows, const long *rowRanks, long nCols, const long *colRanks)
{
    // Permutation has to be set before assembling the system
    if (isAssembled()) {
        throw std::runtime_error("Unable to set the permutations. The system is already assembled.");
    }

    // Reset existing permutations
    resetPermutations();

    // Create new permutations
    PetscInt *rowPermutationsStorage;
    PetscMalloc(nRows * sizeof(PetscInt), &rowPermutationsStorage);
    for (long i = 0; i < nRows; ++i) {
        rowPermutationsStorage[i] = rowRanks[i];
    }

#if BITPIT_ENABLE_MPI == 1
    ISCreateGeneral(m_communicator, nRows, rowPermutationsStorage, PETSC_OWN_POINTER, &m_rowPermutation);
#else
    ISCreateGeneral(PETSC_COMM_SELF, nRows, rowPermutationsStorage, PETSC_OWN_POINTER, &m_rowPermutation);
#endif
    ISSetPermutation(m_rowPermutation);

    PetscInt *colPermutationsStorage;
    PetscMalloc(nCols * sizeof(PetscInt), &colPermutationsStorage);
    for (long j = 0; j < nCols; ++j) {
        colPermutationsStorage[j] = colRanks[j];
    }

#if BITPIT_ENABLE_MPI == 1
    ISCreateGeneral(m_communicator, nCols, colPermutationsStorage, PETSC_OWN_POINTER, &m_colPermutation);
#else
    ISCreateGeneral(PETSC_COMM_SELF, nCols, colPermutationsStorage, PETSC_OWN_POINTER, &m_colPermutation);
#endif
    ISSetPermutation(m_colPermutation);
}

/*!
 * Reset the permutations
 */
void SystemSolver::resetPermutations()
{
    if (m_rowPermutation) {
        ISDestroy(&m_rowPermutation);
    }

    if (m_colPermutation) {
        ISDestroy(&m_colPermutation);
    }
}

/*!
 * Assembly the system.
 *
 * \param matrix is the matrix
 */
void SystemSolver::assembly(const SparseMatrix &matrix)
{
    // Check if the matrix is assembled
    if (!matrix.isAssembled()) {
        throw std::runtime_error("Unable to assembly the system. The matrix is not yet assembled.");
    }

    // Clear the system
    clear();

#if BITPIT_ENABLE_MPI == 1
    // Set the communicator
    setCommunicator(matrix.getCommunicator());

    // Detect if the system is partitioned
    m_partitioned = matrix.isPartitioned();
#endif

    // Initialize matrix
    matrixInit(matrix);
    matrixFill(matrix);

    // Initialize RHS and solution vectors
    vectorsInit();

    // The system is now assembled
    m_assembled = true;
}

/*!
 * Update the system.
 *
 * Only the values of the system matrix can be updated, once the system is
 * assembled its pattern cannot be modified.
 *
 * \param rows are the global indices of the rows that will be updated
 * \param elements are the elements that will be used to update the rows
 */
void SystemSolver::update(const std::vector<long> &rows, const SparseMatrix &elements)
{
    // Check if the element storage is assembled
    if (!elements.isAssembled()) {
        throw std::runtime_error("Unable to update the system. The element storage is not yet assembled.");
    }

    // Check if the system is assembled
    if (!isAssembled()) {
        throw std::runtime_error("Unable to update the system. The system is not yet assembled.");
    }

    // Update matrix
    matrixUpdate(rows, elements);
}

/**
* Get the number of rows of the system.
*
* \result The number of rows of the system.
*/
long SystemSolver::getRowCount() const
{
    if (!isAssembled()) {
        return 0;
    }

    PetscInt nRows;
    MatGetLocalSize(m_A, &nRows, NULL);

    return nRows;
}

/**
* Get the number of columns of the system.
*
* \result The number of columns of the system.
*/
long SystemSolver::getColCount() const
{
    if (!isAssembled()) {
        return 0;
    }

    PetscInt nCols;
    MatGetLocalSize(m_A, NULL, &nCols);

    return nCols;
}

#if BITPIT_ENABLE_MPI==1
/**
* Get the number of global rows
*
* \result The number of global rows
*/
long SystemSolver::getRowGlobalCount() const
{
    if (!isAssembled()) {
        return 0;
    }

    PetscInt nRows;
    MatGetSize(m_A, &nRows, NULL);

    return nRows;
}

/**
* Get number of global columns.
*
* \result The number of global columns.
*/
long SystemSolver::getColGlobalCount() const
{
    if (!isAssembled()) {
        return 0;
    }

    PetscInt nCols;
    MatGetSize(m_A, NULL, &nCols);

    return nCols;
}

/*!
    Checks if the matrix is partitioned.

    \result Returns true if the patch is partitioned, false otherwise.
*/
bool SystemSolver::isPartitioned() const
{
    return m_partitioned;
}
#endif

/*!
 * Check if the system is assembled.
 *
 * \return Returns true if the system is assembled, false otherwise.
 */
bool SystemSolver::isAssembled() const
{
    return m_assembled;
}

/*!
 * Check if the system is set up.
 *
 * \return Returns true if the system is set up, false otherwise.
 */
bool SystemSolver::isSetUp() const
{
    return m_setUp;
}

/*!
 * Solve the system
 */
void SystemSolver::solve()
{
    // Check if the system is assembled
    if (!isAssembled()) {
        throw std::runtime_error("Unable to solve the system. The system is not yet assembled.");
    }

    // Check if the system is set up
    if (!isSetUp()) {
        setUp();
    }

    // Perfrom actions before KSP solution
    preKSPSolveActions();

    // Solve the system
    m_KSPStatus.error = KSPSolve(m_KSP, m_rhs, m_solution);

    // Set solver info
    if (m_KSPStatus.error == 0) {
        KSPGetIterationNumber(m_KSP, &m_KSPStatus.its);
        KSPGetConvergedReason(m_KSP, &m_KSPStatus.convergence);
    } else {
        m_KSPStatus.its         = -1;
        m_KSPStatus.convergence = KSP_DIVERGED_BREAKDOWN;
    }

    // Perfrom actions after KSP solution
    postKSPSolveActions();
}

/*!
 * Solve the system
 *
 * \param rhs is the right-hand-side of the system
 * \param solution in input should contain the initial solution, on output it
 * contains the solution of the linear system
 */
void SystemSolver::solve(const std::vector<double> &rhs, std::vector<double> *solution)
{
    // Fills the vectors
    vectorsFill(rhs, solution);

    // Solve the system
    solve();

    // Export the solution
    vectorsExport(solution);
}

/*!
 * Pre-solve actions.
 */
void SystemSolver::preKSPSolveActions()
{
    // Apply permutations
    vectorsPermute(false);
}

/*!
 * Post-solve actions.
 */
void SystemSolver::postKSPSolveActions()
{
    // Invert permutations
    vectorsPermute(true);
}

/*!
 * Initializes the matrix.
 *
 * \param matrix is the matrix
 */
void SystemSolver::matrixInit(const SparseMatrix &matrix)
{
    long nRows = matrix.getRowCount();
    long nCols = matrix.getColCount();

    const PetscInt *rowRanks = nullptr;
    if (m_rowPermutation) {
        ISGetIndices(m_rowPermutation, &rowRanks);
    }

    // Set row and column global offset
#if BITPIT_ENABLE_MPI == 1
    m_rowGlobalOffset = matrix.getRowGlobalOffset();
    m_colGlobalOffset = matrix.getColGlobalOffset();
#endif

#if BITPIT_ENABLE_MPI == 1
    // Evaluate the number of non-zero elements
    //
    // For each row we count the number of local non-zero elements (d_nnz) and
    // the number of non-zero elements that belong to other processors (o_nnz)
    long nGlobalCols      = matrix.getColGlobalCount();
    long nOffDiagonalCols = nGlobalCols - nCols;
    long firstColGlobalId = matrix.getColGlobalOffset();
    long lastColGlobalId  = firstColGlobalId + nCols - 1;

    std::vector<int> d_nnz(nRows, 0);
    std::vector<int> o_nnz(nRows, 0);
    if (nOffDiagonalCols > 0) {
        for (long row = 0; row < nRows; ++row) {
            long matrixRow = row;
            if (m_rowPermutation) {
                matrixRow = rowRanks[matrixRow];
            }

            ConstProxyVector<long> rowPattern = matrix.getRowPattern(matrixRow);
            int nRowNZ = rowPattern.size();
            for (int k = 0; k < nRowNZ; ++k) {
                long columnGlobalId = rowPattern[k];
                if (columnGlobalId < firstColGlobalId || columnGlobalId > lastColGlobalId) {
                    ++o_nnz[row];
                } else {
                    ++d_nnz[row];
                }
            }
        }
    } else {
        for (long row = 0; row < nRows; ++row) {
            long matrixRow = row;
            if (m_rowPermutation) {
                matrixRow = rowRanks[matrixRow];
            }

            ConstProxyVector<long> rowPattern = matrix.getRowPattern(matrixRow);
            d_nnz[row] = rowPattern.size();
        }
    }

    // Create the matrix
    MatCreateAIJ(m_communicator, nRows, nCols, PETSC_DETERMINE, PETSC_DETERMINE, 0, d_nnz.data(), 0, o_nnz.data(), &m_A);
#else
    // Evaluate the number of non-zero elements
    std::vector<int> d_nnz(nCols);

    for (long row = 0; row < nRows; ++row) {
        long matrixRow = row;
        if (m_rowPermutation) {
            matrixRow = rowRanks[matrixRow];
        }

        ConstProxyVector<long> rowPattern = matrix.getRowPattern(matrixRow);
        d_nnz[row] = rowPattern.size();
    }

    // Create the matrix
    MatCreateSeqAIJ(PETSC_COMM_SELF, nRows, nCols, 0, d_nnz.data(), &m_A);
#endif

    // Cleanup
    if (m_rowPermutation) {
        ISRestoreIndices(m_rowPermutation, &rowRanks);
    }
}

/*!
 * Fills the matrix.
 *
 * \param matrix is the matrix
 */
void SystemSolver::matrixFill(const SparseMatrix &matrix)
{
    const long nRows = matrix.getRowCount();
    const long nCols = matrix.getColCount();
    const long maxRowNZ = matrix.getMaxRowNZCount();

    const PetscInt *rowRanks = nullptr;
    if (m_rowPermutation) {
        ISGetIndices(m_rowPermutation, &rowRanks);
    }

    IS invColPermutation;
    const PetscInt *colInvRanks = nullptr;
    if (m_colPermutation) {
        ISInvertPermutation(m_colPermutation, nCols, &invColPermutation);
        ISGetIndices(invColPermutation, &colInvRanks);
    }

    // Create the matrix
    if (maxRowNZ > 0) {
        std::vector<PetscInt> rowNZGlobalIds(maxRowNZ);
        std::vector<PetscScalar> rowNZValues(maxRowNZ);

        long rowGlobalOffset;
#if BITPIT_ENABLE_MPI == 1
        rowGlobalOffset = m_rowGlobalOffset;
#else
        rowGlobalOffset = 0;
#endif

        long firstGlobalCol = rowGlobalOffset;
        long lastGlobalCol  = firstGlobalCol + nCols - 1;

        for (long row = 0; row < nRows; ++row) {
            long matrixRow = row;
            if (m_rowPermutation) {
                matrixRow = rowRanks[matrixRow];
            }

            ConstProxyVector<long> rowPattern = matrix.getRowPattern(matrixRow);
            ConstProxyVector<double> rowValues = matrix.getRowValues(matrixRow);

            const int nRowNZ = rowPattern.size();
            const PetscInt globalRow = rowGlobalOffset + row;
            for (int k = 0; k < nRowNZ; ++k) {
                long matrixGlobalCol = rowPattern[k];

                long globalCol = matrixGlobalCol;
                if (m_colPermutation) {
                    if (globalCol >= firstGlobalCol && globalCol <= lastGlobalCol) {
                        long col = globalCol - firstGlobalCol;
                        col = colInvRanks[col];
                        globalCol = firstGlobalCol + col;
                    }
                }

                rowNZGlobalIds[k] = globalCol;
                rowNZValues[k]    = rowValues[k];
            }

            MatSetValues(m_A, 1, &globalRow, nRowNZ, rowNZGlobalIds.data(), rowNZValues.data(), INSERT_VALUES);
        }
    }

    // Let petsc build the matrix
    MatAssemblyBegin(m_A, MAT_FINAL_ASSEMBLY);
    MatAssemblyEnd(m_A, MAT_FINAL_ASSEMBLY);

    // Cleanup
    if (m_rowPermutation) {
        ISRestoreIndices(m_rowPermutation, &rowRanks);
    }

    if (m_colPermutation) {
        ISDestroy(&invColPermutation);
    }
}

/*!
 * Update the specified rows of the matrix.
 *
 * The contents of the specified rows will be replaced by the specified
 * elements.
 *
 * \param rows are the global indices of the rows that will be updated
 * \param elements are the elements that will be used to update the rows
 */
void SystemSolver::matrixUpdate(const std::vector<long> &rows, const SparseMatrix &elements)
{
    const long maxRowElements = std::max(elements.getMaxRowNZCount(), 0L);

    // Check if element columns are already in the pattern
    std::unordered_set<PetscInt> currentRowPattern;

    long rowGlobalOffset;
#if BITPIT_ENABLE_MPI == 1
    rowGlobalOffset = m_rowGlobalOffset;
#else
    rowGlobalOffset = 0;
#endif

    for (std::size_t n = 0; n < rows.size(); ++n) {
        ConstProxyVector<long> rowPattern = elements.getRowPattern(n);
        const int nRowElements = rowPattern.size();
        if (nRowElements == 0) {
            continue;
        }

        // Get global row
        long row = rows[n];
        const PetscInt globalRow = rowGlobalOffset + row;

        // Get current row pattern
        PetscInt nCurrentRowElements = 0;
        const PetscInt *rawCurrentRowPattern = nullptr;
        MatGetRow(m_A, globalRow, &nCurrentRowElements, &rawCurrentRowPattern, NULL);
        assert(nCurrentRowElements != 0);
        assert(rawCurrentRowPattern != nullptr);

        currentRowPattern.clear();
        for (PetscInt k = 0; k < nCurrentRowElements; ++k) {
            currentRowPattern.insert(rawCurrentRowPattern[k]);
        }

        // Check if element columns are already in the pattern
        for (int k = 0; k < nRowElements; ++k) {
            if (currentRowPattern.count(rowPattern[k]) == 0) {
                throw std::runtime_error("The element is not in the matrix.");
            }
        }

        // Restore row
        MatRestoreRow(m_A, globalRow, &nCurrentRowElements, &rawCurrentRowPattern, NULL);
    }

    // Update element values
    std::vector<PetscInt> rawRowPattern(maxRowElements);
    std::vector<PetscScalar> rawRowValues(maxRowElements);

    for (std::size_t n = 0; n < rows.size(); ++n) {
        ConstProxyVector<double> rowValues = elements.getRowValues(n);
        const int nRowElements = rowValues.size();
        if (nRowElements == 0) {
            continue;
        }

        // Get global row
        long row = rows[n];
        const PetscInt globalRow = rowGlobalOffset + row;

        // Get pattern
        ConstProxyVector<long> rowPattern = elements.getRowPattern(n);

        // Update values
        for (int k = 0; k < nRowElements; ++k) {
            rawRowPattern[k] = rowPattern[k];;
            rawRowValues[k]  = rowValues[k];
        }

        MatSetValues(m_A, 1, &globalRow, nRowElements, rawRowPattern.data(), rawRowValues.data(), INSERT_VALUES);
    }

    // Let petsc assembly the matrix after the update
    MatAssemblyBegin(m_A, MAT_FINAL_ASSEMBLY);
    MatAssemblyEnd(m_A, MAT_FINAL_ASSEMBLY);
}

/*!
 * Initialize rhs and solution vectors.
 */
void SystemSolver::vectorsInit()
{
    PetscInt nRows;
    PetscInt nColumns;
    MatGetLocalSize(m_A, &nRows, &nColumns);

#if BITPIT_ENABLE_MPI == 1
    PetscInt nGlobalRows;
    PetscInt nGlobalColumns;
    MatGetSize(m_A, &nGlobalRows, &nGlobalColumns);

    PetscInt nGhosts;
    const PetscInt *ghosts;
    MatGetGhosts(m_A, &nGhosts, &ghosts);

    VecCreateGhost(m_communicator, nColumns, nGlobalColumns, nGhosts, ghosts, &m_solution);
    VecCreateGhost(m_communicator, nRows, nGlobalRows, nGhosts, ghosts, &m_rhs);
#else
    VecCreateSeq(PETSC_COMM_SELF, nColumns, &m_solution);
    VecCreateSeq(PETSC_COMM_SELF, nRows, &m_rhs);
#endif
}

/*!
 * Apply permutations to RHS and solution vectors.
 *
 * \param invert is a flag for inverting the permutation
 */
void SystemSolver::vectorsPermute(bool invert)
{
    PetscBool petscInvert;
    if (invert) {
        petscInvert = PETSC_TRUE;
    } else {
        petscInvert = PETSC_FALSE;
    }

    if (m_colPermutation) {
        VecPermute(m_solution, m_colPermutation, petscInvert);
    }

    if (m_rowPermutation) {
        VecPermute(m_rhs, m_rowPermutation, petscInvert);
    }
}

/*!
 * Fills rhs and solution vectors.
 *
 * \param rhs is the right-hand-side of the system
 * \param solution is the solution of the linear system
 */
void SystemSolver::vectorsFill(const std::vector<double> &rhs, std::vector<double> *solution)
{
    // Import RHS
    int nRows;
    VecGetLocalSize(m_rhs, &nRows);

    PetscScalar *raw_rhs;
    VecGetArray(m_rhs, &raw_rhs);
    for (int i = 0; i < nRows; ++i) {
        raw_rhs[i] = rhs[i];
    }
    VecRestoreArray(m_rhs, &raw_rhs);

    // Import initial solution
    int nColumns;
    VecGetLocalSize(m_solution, &nColumns);

    PetscScalar *raw_solution;
    VecGetArray(m_solution, &raw_solution);
    for (int i = 0; i < nColumns; ++i) {
        raw_solution[i] = (*solution)[i];
    }
    VecRestoreArray(m_solution, &raw_solution);
}

/*!
 * Export the solution vector.
 *
 * \param solution on output it will contain the solution of the linear system
 */
void SystemSolver::vectorsExport(std::vector<double> *solution)
{
    int size;
    VecGetLocalSize(m_solution, &size);

    const PetscScalar *raw_solution;
    VecGetArrayRead(m_solution, &raw_solution);
    for (int i = 0; i < size; ++i) {
        (*solution)[i] = raw_solution[i];
    }
    VecRestoreArrayRead(m_solution, &raw_solution);
}

/*!
 * Get a raw pointer to the solution vector.
 *
 * \result A raw pointer to the solution vector.
 */
double * SystemSolver::getRHSRawPtr()
{
    PetscScalar *raw_rhs;
    VecGetArray(m_rhs, &raw_rhs);

    return raw_rhs;
}

/*!
 * Get a constant raw pointer to the solution vector.
 *
 * \result A constant raw pointer to the solution vector.
 */
const double * SystemSolver::getRHSRawPtr() const
{
    return getRHSRawReadPtr();
}

/*!
 * Get a constant raw pointer to the solution vector.
 *
 * \result A constant raw pointer to the solution vector.
 */
const double * SystemSolver::getRHSRawReadPtr() const
{
    const PetscScalar *raw_rhs;
    VecGetArrayRead(m_rhs, &raw_rhs);

    return raw_rhs;
}

/*!
 * Restores the solution vector after getRHSRawPtr() has been called.
 *
 * \param raw_rhs is the location of pointer to array obtained from
 * getRHSRawPtr()
 */
void SystemSolver::restoreRHSRawPtr(double *raw_rhs)
{
    VecRestoreArray(m_rhs, &raw_rhs);
}

/*!
 * Restores the solution vector after getRHSRawReadPtr() has been called.
 *
 * \param raw_rhs is the location of pointer to array obtained from
 * getRHSRawReadPtr()
 */
void SystemSolver::restoreRHSRawReadPtr(const double *raw_rhs) const
{
    VecRestoreArrayRead(m_rhs, &raw_rhs);
}

/*!
 * Get a raw pointer to the solution vector.
 *
 * \result A raw pointer to the solution vector.
 */
double * SystemSolver::getSolutionRawPtr()
{
    PetscScalar *raw_solution;
    VecGetArray(m_solution, &raw_solution);

    return raw_solution;
}

/*!
 * Get a constant raw pointer to the solution vector.
 *
 * \result A constant raw pointer to the solution vector.
 */
const double * SystemSolver::getSolutionRawPtr() const
{
    return getSolutionRawReadPtr();
}

/*!
 * Get a constant raw pointer to the solution vector.
 *
 * \result A constant raw pointer to the solution vector.
 */
const double * SystemSolver::getSolutionRawReadPtr() const
{
    const PetscScalar *raw_solution;
    VecGetArrayRead(m_solution, &raw_solution);

    return raw_solution;
}

/*!
 * Restores the solution vector after getSolutionRawPtr() has been called.
 *
 * \param raw_solution is the location of pointer to array obtained from
 * getSolutionRawPtr()
 */
void SystemSolver::restoreSolutionRawPtr(double *raw_solution)
{
    VecRestoreArray(m_solution, &raw_solution);
}

/*!
 * Restores the solution vector after getSolutionRawReadPtr() has been called.
 *
 * \param raw_solution is the location of pointer to array obtained from
 * getSolutionRawReadPtr()
 */
void SystemSolver::restoreSolutionRawReadPtr(const double *raw_solution) const
{
    VecRestoreArrayRead(m_solution, &raw_solution);
}

/*!
 * Dump the system to file
 *
 * \param directory is the directory where the files will be saved
 * \param prefix is the prefix that will be added to the files
 * \param matrixFormat is the dump format that will be used for the matrix,
 * the ASCII format may not be able to dump large matrices
 * \param rhsFormat is the dump format that will be used for the RHS,
 * the ASCII format may not be able to dump large vectors
 * \param solutionFormat is the dump format that will be used for the solution,
 * the ASCII format may not be able to dump large vectors
 */
void SystemSolver::dump(const std::string &directory, const std::string &prefix,
                        DumpFormat matrixFormat, DumpFormat rhsFormat,
                        DumpFormat solutionFormat) const
{
    std::stringstream filePathStream;

    // Matrix
    PetscViewerType matrixViewerType;
    PetscViewerFormat matrixViewerFormat;
    if (matrixFormat == DUMP_BINARY) {
        matrixViewerType   = PETSCVIEWERBINARY;
        matrixViewerFormat = PETSC_VIEWER_DEFAULT;
    } else {
        matrixViewerType   = PETSCVIEWERASCII;
        matrixViewerFormat = PETSC_VIEWER_ASCII_MATLAB;
    }

    PetscViewer matViewer;
#if BITPIT_ENABLE_MPI==1
    PetscViewerCreate(m_communicator, &matViewer);
#else
    PetscViewerCreate(PETSC_COMM_SELF, &matViewer);
#endif
    PetscViewerSetType(matViewer, matrixViewerType);
    PetscViewerFileSetMode(matViewer, FILE_MODE_WRITE);
    PetscViewerPushFormat(matViewer, matrixViewerFormat);

    filePathStream.str(std::string());
    filePathStream << directory << "/" << prefix << "A.txt";
    PetscViewerFileSetName(matViewer, filePathStream.str().c_str());
    MatView(m_A, matViewer);
    PetscViewerDestroy(&matViewer);

    // RHS
    PetscViewerType rhsViewerType;
    PetscViewerFormat rhsViewerFormat;
    if (rhsFormat == DUMP_BINARY) {
        rhsViewerType   = PETSCVIEWERBINARY;
        rhsViewerFormat = PETSC_VIEWER_DEFAULT;
    } else {
        rhsViewerType   = PETSCVIEWERASCII;
        rhsViewerFormat = PETSC_VIEWER_ASCII_MATLAB;
    }

    PetscViewer rhsViewer;
#if BITPIT_ENABLE_MPI==1
    PetscViewerCreate(m_communicator, &rhsViewer);
#else
    PetscViewerCreate(PETSC_COMM_SELF, &rhsViewer);
#endif
    PetscViewerSetType(rhsViewer, rhsViewerType);
    PetscViewerFileSetMode(rhsViewer, FILE_MODE_WRITE);
    PetscViewerPushFormat(rhsViewer, rhsViewerFormat);

    filePathStream.str(std::string());
    filePathStream << directory << "/" << prefix << "rhs.txt";
    PetscViewerFileSetName(rhsViewer, filePathStream.str().c_str());
    VecView(m_rhs, rhsViewer);
    PetscViewerDestroy(&rhsViewer);

    // Solution
    PetscViewerType solutionViewerType;
    PetscViewerFormat solutionViewerFormat;
    if (solutionFormat == DUMP_BINARY) {
        solutionViewerType   = PETSCVIEWERBINARY;
        solutionViewerFormat = PETSC_VIEWER_DEFAULT;
    } else {
        solutionViewerType   = PETSCVIEWERASCII;
        solutionViewerFormat = PETSC_VIEWER_ASCII_MATLAB;
    }

    PetscViewer solutionViewer;
#if BITPIT_ENABLE_MPI==1
    PetscViewerCreate(m_communicator, &solutionViewer);
#else
    PetscViewerCreate(PETSC_COMM_SELF, &solutionViewer);
#endif
    PetscViewerSetType(solutionViewer, solutionViewerType);
    PetscViewerFileSetMode(solutionViewer, FILE_MODE_WRITE);
    PetscViewerPushFormat(solutionViewer, solutionViewerFormat);

    filePathStream.str(std::string());
    filePathStream << directory << "/" << prefix << "solution.txt";
    PetscViewerFileSetName(solutionViewer, filePathStream.str().c_str());
    VecView(m_solution, solutionViewer);
    PetscViewerDestroy(&solutionViewer);
}

/*!
 * Attaches a null space to the system matrix.
 */
void SystemSolver::setNullSpace()
{
    MatNullSpace nullspace;
#if BITPIT_ENABLE_MPI==1
    MatNullSpaceCreate(m_communicator, PETSC_TRUE, 0, NULL, &nullspace);
#else
    MatNullSpaceCreate(PETSC_COMM_SELF, PETSC_TRUE, 0, NULL, &nullspace);
#endif
    MatSetNullSpace(m_A, nullspace);
    MatNullSpaceDestroy(&nullspace);
}

/*!
 * Removes the null space from the system matrix.
 */
void SystemSolver::unsetNullSpace()
{
    MatSetNullSpace(m_A, NULL);
}

/*!
 * Setup the system.
 */
void SystemSolver::setUp()
{
    // Check if the system is assembled
    if (!isAssembled()) {
        throw std::runtime_error("Unable to solve the system. The system is not yet assembled.");
    }

    // Destroy existing Krylov space
    if (m_KSP) {
        KSPDestroy(&m_KSP);
    }

    // Set options prefix
    if (!m_prefix.empty()) {
        KSPSetOptionsPrefix(m_KSP, m_prefix.c_str());
    }

    // Create Krylov space
#if BITPIT_ENABLE_MPI==1
    KSPCreate(m_communicator, &m_KSP);
#else
    KSPCreate(PETSC_COMM_SELF, &m_KSP);
#endif

    KSPSetOperators(m_KSP, m_A, m_A);

    // Perform actions before KSP set up
    preKSPSetupActions();

    // Setup Krylov space
    m_optionsEditable = false;
    KSPSetFromOptions(m_KSP);
    KSPSetUp(m_KSP);

    // Perform actions after KSP set up
    postKSPSetupActions();
}

/*!
 * Perform actions before KSP setup.
 */
void SystemSolver::preKSPSetupActions()
{
    // Preconditioner configuration
    PCType preconditionerType;
#if BITPIT_ENABLE_MPI == 1
    if (isPartitioned()) {
        preconditionerType = PCASM;
    } else {
        preconditionerType = PCILU;
    }
#else
    preconditionerType = PCILU;
#endif

    PC preconditioner;
    KSPGetPC(m_KSP, &preconditioner);
    PCSetType(preconditioner, preconditionerType);
    if (strcmp(preconditionerType, PCASM) == 0) {
        if (m_KSPOptions.overlap != PETSC_DEFAULT) {
            PCASMSetOverlap(preconditioner, m_KSPOptions.overlap);
        }
    } else if (strcmp(preconditionerType, PCILU) == 0) {
        if (m_KSPOptions.levels != PETSC_DEFAULT) {
            PCFactorSetLevels(preconditioner, m_KSPOptions.levels);
        }
    }

    // Solver configuration
    KSPSetType(m_KSP, KSPFGMRES);
    if (m_KSPOptions.restart != PETSC_DEFAULT) {
        KSPGMRESSetRestart(m_KSP, m_KSPOptions.restart);
    }
    if (m_KSPOptions.rtol != PETSC_DEFAULT || m_KSPOptions.maxits != PETSC_DEFAULT) {
        KSPSetTolerances(m_KSP, m_KSPOptions.rtol, PETSC_DEFAULT, PETSC_DEFAULT, m_KSPOptions.maxits);
    }
    KSPSetInitialGuessNonzero(m_KSP, PETSC_TRUE);
}

/*!
 * Perform actions after KSP setup.
 */
void SystemSolver::postKSPSetupActions()
{
    // Get preconditioner information
    PC preconditioner;
    KSPGetPC(m_KSP, &preconditioner);

    PCType preconditionerType;
    PCGetType(preconditioner, &preconditionerType);

    // Set ASM sub block preconditioners
    if (strcmp(preconditionerType, PCASM) == 0) {
        KSP *subksp;
        PC subpc;
        PetscInt nlocal, first;
        PCASMGetSubKSP(preconditioner, &nlocal, &first, &subksp);
        for (PetscInt i = 0; i < nlocal; ++i) {
            KSPGetPC(subksp[i], &subpc);
            PCSetType(subpc, PCILU);
            if (m_KSPOptions.sublevels != PETSC_DEFAULT) {
                PCFactorSetLevels(subpc, m_KSPOptions.sublevels);
            }
            if (m_KSPOptions.subrtol != PETSC_DEFAULT) {
                KSPSetTolerances(subksp[i], m_KSPOptions.subrtol, PETSC_DEFAULT, PETSC_DEFAULT, PETSC_DEFAULT);
            }
        }
    }
}

/*!
 * Get a reference to the options associated to the Kryolov solver.
 *
 * \return A reference to the options associated to the Kryolov solver.
 */
KSPOptions & SystemSolver::getKSPOptions()
{
    return m_KSPOptions;
}

/*!
 * Get a constant reference to the options associated to the Kryolov solver.
 *
 * \return A constant reference to the options associated to the Kryolov solver.
 */
const KSPOptions & SystemSolver::getKSPOptions() const
{
    return m_KSPOptions;
}

/*!
 * Get a constant reference to the status of the Kryolov solver.
 *
 * \return A constant reference to the status of the Kryolov solver.
 */
const KSPStatus & SystemSolver::getKSPStatus() const
{
    return m_KSPStatus;
}

#if BITPIT_ENABLE_MPI==1
/*!
	Gets the MPI communicator associated to the system.

	\return The MPI communicator associated to the system.
*/
const MPI_Comm & SystemSolver::getCommunicator() const
{
	return m_communicator;
}

/*!
	Sets the MPI communicator to be used for parallel communications.

	\param communicator is the communicator to be used for parallel
	communications.
*/
void SystemSolver::setCommunicator(MPI_Comm communicator)
{
    if ((communicator != MPI_COMM_NULL) && (communicator != MPI_COMM_SELF)) {
        MPI_Comm_dup(communicator, &m_communicator);
    } else {
        m_communicator = MPI_COMM_SELF;
    }
}

/*!
	Frees the MPI communicator associated to the matrix.
*/
void SystemSolver::freeCommunicator()
{
    if (m_communicator != MPI_COMM_SELF) {
        int finalizedCalled;
        MPI_Finalized(&finalizedCalled);
        if (!finalizedCalled) {
            MPI_Comm_free(&m_communicator);
        }
    }
}
#endif

}
