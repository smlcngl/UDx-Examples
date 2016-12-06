/* Copyright (c) 2005 - 2016 Hewlett Packard Enterprise Development LP  -*- C++ -*-*/
/****************************
 * Vertica Analytic Database
 *
 * UDL helper/wrapper; allows continuous reading and writing of data,
 * rather than the state-machine-based approach in the stock API.
 * Parser implementation.
 *
 ****************************/

#include "CoroutineHelpers.h"

#ifndef CONTINUOUSUDFILTER_H_
#define CONTINUOUSUDFILTER_H_

/**
 * ContinuousUDFilter
 *
 * This is a wrapper on top of the UDFilter class.  It provides an abstraction
 * that allows user code to treat the output as a continuous stream of data,
 * that they can write to at will and process in a continuous manner
 * rather than having an iterator-like design where a user function is called
 * repeatedly to get more data.
 */
class ContinuousUDFilter : public Vertica::UDFilter {
public:
    // Functions to implement

    /**
     * ContinuousUDFilter::initialize()
     *
     * Will be invoked during query execution, prior to run() being called.
     *
     * May optionally be overridden to perform setup/initialzation.
     */
    virtual void initialize(Vertica::ServerInterface &srvInterface) {}

    /**
     * ContinuousUDFilter::run()
     *
     * User-implemented method that processes data.
     * Called exactly once per ContinuousUDFilter instance.
     * It should read and write data using the reserve() and seek()
     * (or read()) methods on the 'cr' and 'cw' fields, respectively.
     * It should return once it has either finished processing the
     * input stream, or it (for whatever reason) wants to close the
     * input stream and not process any further data.
     *
     * run() should be very careful about keeping pointers or references
     * to internal values returned by methods on the ContinuousUDParser
     * class.  Unless documentation explicitly indicates that it is
     * safe to do so, it is not safe to keep such pointers or references,
     * even for objects (such as the server interface) that one might
     * otherwise expect to be persistent.  Instead, call the accessor
     * function for each use.
     */
    virtual void run() {}

    /**
     * ContinuousUDFilter::deinitialize()
     *
     * Will be invoked during query execution, after run() has returned.
     *
     * May optionally be overridden to perform tear-down/destruction.
     */
    virtual void deinitialize(Vertica::ServerInterface &srvInterface) {}

protected:
    // Functions to use

    /**
     * Get the current ServerInterface.
     *
     * Do not store the return value of this function.  Other
     * function calls on ContinuousUDParser may change
     * the server interface that it returns.
     */
    Vertica::ServerInterface& getServerInterface() { return *srvInterface; }

    /**
     * Return control to the server.
     * Use this method in idle or busy loops, to allow the server to
     * check for status changes or query cancellations.
     */
    void yield() {
        state = RUN_START;
        c.switchBack();
    }

    /**
     * ContinuousReader
     * Houses methods relevant to reading raw binary buffers
     */
    ContinuousReader cr;

    /**
     * ContinuousWriter
     * Houses methods relevant to writing raw binary buffers
     */
    ContinuousWriter cw;

private:

    /****************************************************
     ****************************************************
     ****************************************************
     *
     * All code below this point is a part of the internal implementation of
     * this class.  If you are simply trying to write a ContinuousUDParser,
     * it is unnecessary to read beyond this point.
     *
     * However, the following is also presented as example code, and can be
     * taken and modified if/as needed.
     *
     ***************************************************
     ***************************************************
     ***************************************************/

    Vertica::ServerInterface *srvInterface;

    std::auto_ptr<udf_exception> exception;

    Coroutine c;

    /** State of the internal worker context */
    enum State
    {
        NOT_INITIALIZED, // Coroutine context not in existence
        RUN_START,    // Coroutine running
        FINISHED,     // Coroutine has declared that it's done processing data
        CLOSED,       // Coroutine abort finished
    } state;

protected:
    static int _ContinuousUDFilterRunner(ContinuousUDFilter *source) {
        // Internal error, haven't set a parser but are trying to run it
        VIAssert(source != NULL);

        try {
            source->runHelper();
        } catch (udf_exception &e) {
            source->exception.reset(new udf_exception(e));
        }

        return 0;
    }

    /**
     * Called by _ContinuousUDParserRunner() to handle immediate
     * set-up and tear-down for run()
     */
    void runHelper() {
        run();
        state = FINISHED;
    }

public:
    // Constructor.  Initialize stuff properly.
    // In particular, various members need access to our Coroutine.
    // Also, initialize POD types to
    ContinuousUDFilter() : cr(c), cw(c), srvInterface(NULL) {}

    // Wrap UDParser::setup(); we have some initialization of our own to do
    void setup(Vertica::ServerInterface &srvInterface) {
        state = NOT_INITIALIZED;

        // Set as many of (srvInterface, currentBuffer, inputState)
        // to NULL for as much of the time as possible.
        // Makes things more fail-fast, if someone tries to use
        // one of them and it's not currently defined/valid,
        // since they are invalidated frequently.
        this->srvInterface = &srvInterface;

        c.initializeNewContext(&getServerInterface());

        initialize(srvInterface);

        // Initialize the coroutine stack
        c.start((int(*)(void*))_ContinuousUDFilterRunner, (void*)this);
        state = RUN_START;

        this->srvInterface = NULL;
    }

    // Wrap UDParser::destroy(); we have some tear-down of our own to do
    void destroy(Vertica::ServerInterface &srvInterface) {
        this->srvInterface = &srvInterface;
        deinitialize(srvInterface);
        this->srvInterface = NULL;
        state = CLOSED;
    }

    /**
     * Override the built-in process() method with our own logic.
     * Abstract away the state-machine interface by stuffing run() into a
     * coroutine.
     * Whenever run() wants more data than we have right now,
     * context-switch out of the coroutine and back into process(), and
     * go get whatever run() needs.
     */
    Vertica::StreamState process(Vertica::ServerInterface &srvInterface,
            Vertica::DataBuffer &input, Vertica::InputState input_state,
            Vertica::DataBuffer &output) {
        // Capture the new state for this run
        // IMPORTANT:  It is unsafe to access any of these values outside
        // of this function call!
        // So be careful that run() is never running while we're not inside
        // a process() call.
        this->srvInterface = &srvInterface;
        this->cr.currentBuffer = &input;
        this->cr.state = &input_state;
        this->cw.currentBuffer = &output;
        // Fake output state
        Vertica::InputState output_state = Vertica::OK;
        this->cw.state = &output_state;

        // setup() is supposed to take care of getting us out of this state
        VIAssert(state != NOT_INITIALIZED);

        // Pass control to the user.
        c.switchIntoCoroutine();

        // Don't need these any more; clear them to make sure they
        // don't get used improperly
        this->srvInterface = NULL;
        this->cr.currentBuffer = NULL;
        this->cr.state = NULL;
        this->cw.currentBuffer = NULL;
        this->cw.state = NULL;

        // Propagate exception.
        if (exception.get()) throw *exception;

        if (this->cr.needInput) {
            this->cr.needInput = false;
            return Vertica::INPUT_NEEDED;
        }

        if (this->cw.needInput) {
            this->cw.needInput = false;
            return Vertica::OUTPUT_NEEDED;
        }

        if (state == RUN_START) {
            return Vertica::KEEP_GOING;
        }

        // We should only be able to get here if we're finished
        VIAssert(state == FINISHED);
        return Vertica::DONE;
    }
};


#endif  // CONTINUOUSUDFILTER_H_
