#include <pv/pvDatabase.h>
#include <pv/nt.h>
#include <pv/channelProviderLocal.h>

#include <epicsThread.h>
#include <epicsExport.h>
#include <iocsh.h>

#include <asynDriver.h>

#include <stdlib.h>
#include <vector>
#include <string.h>
#include <stdio.h>

#include <math.h>

#include "NDPluginDriver.h"
#include "NDPluginV4Server.h"

using namespace epics;
using namespace epics::pvData;
using namespace epics::pvAccess;
using namespace epics::pvDatabase;
using namespace epics::nt;
using namespace std;
using tr1::static_pointer_cast;
using tr1::dynamic_pointer_cast;

static const PVDataCreatePtr PVDC = getPVDataCreate();

template <typename arrayType, typename srcDataType>
static void copyValue (PVUnionPtr dest, void *src, size_t count)
{
    string unionField = ScalarTypeFunc::name(arrayType::typeCode);
    unionField += "Value";

    tr1::shared_ptr<arrayType> pvField = dest->select<arrayType>(unionField);

    shared_vector<typename arrayType::value_type> temp(pvField->reuse());
    temp.resize(count);

    srcDataType *data = (srcDataType*)src;
    std::copy(data, data + count, temp.begin());
    pvField->replace(freeze(temp));

    dest->postPut();
}

static void copyValue (NTNDArrayPtr dest, NDArray *src)
{
    NDArrayInfo_t arrayInfo;
    size_t count;
    PVUnionPtr destData = dest->getValue();
    void *srcData = src->pData;

    src->getInfo(&arrayInfo);
    count = arrayInfo.nElements;

    switch(src->dataType)
    {
    case NDInt8:    copyValue<PVByteArray, int8_t> (destData, srcData, count);     break;
    case NDUInt8:   copyValue<PVUByteArray, uint8_t> (destData, srcData, count);   break;
    case NDInt16:   copyValue<PVShortArray, int16_t> (destData, srcData, count);   break;
    case NDUInt16:  copyValue<PVUShortArray, uint16_t> (destData, srcData, count); break;
    case NDInt32:   copyValue<PVIntArray, int32_t> (destData, srcData, count);     break;
    case NDUInt32:  copyValue<PVUIntArray, uint32_t> (destData, srcData, count);   break;
    case NDFloat32: copyValue<PVFloatArray, float> (destData, srcData, count);     break;
    case NDFloat64: copyValue<PVDoubleArray, double> (destData, srcData, count);   break;
    }
}

template <typename pvAttrType, typename valueType>
static void copyAttribute (PVStructurePtr dest, NDAttribute *src)
{
    valueType value;
    src->getValue(src->getDataType(), (void*)&value);

    PVUnionPtr valueFieldUnion = dest->getSubField<PVUnion>("value");
    static_pointer_cast<pvAttrType>(valueFieldUnion->get())->put(value);
}

static void copyStringAttribute (PVStructurePtr dest, NDAttribute *src)
{
    NDAttrDataType_t attrDataType;
    size_t attrDataSize;

    src->getValueInfo(&attrDataType, &attrDataSize);

    char value[attrDataSize];
    src->getValue(attrDataType, value, attrDataSize);

    PVUnionPtr valueFieldUnion = dest->getSubField<PVUnion>("value");
    static_pointer_cast<PVString>(valueFieldUnion->get())->put(value);
}

static void createAttributes (PVStructureArrayPtr dest, NDAttributeList *src)
{
    NDAttribute *attr = src->next(NULL);
    PVStructureArray::svector destVec(dest->reuse());

    while(attr)
    {
        PVStructurePtr pvAttr;
        pvAttr = PVDC->createPVStructure(dest->getStructureArray()->getStructure());

        pvAttr->getSubField<PVString>("name")->put(attr->getName());
        pvAttr->getSubField<PVString>("descriptor")->put(attr->getDescription());
        pvAttr->getSubField<PVString>("source")->put(attr->getSource());

        NDAttrSource_t sourceType;
        attr->getSourceInfo(&sourceType);
        pvAttr->getSubField<PVInt>("sourceType")->put(sourceType);

        PVUnionPtr valueField = pvAttr->getSubField<PVUnion>("value");
        switch(attr->getDataType())
        {

        case NDAttrInt8:    valueField->set(PVDC->createPVScalar<PVByte>());   break;
        case NDAttrUInt8:   valueField->set(PVDC->createPVScalar<PVUByte>());  break;
        case NDAttrInt16:   valueField->set(PVDC->createPVScalar<PVShort>());  break;
        case NDAttrUInt16:  valueField->set(PVDC->createPVScalar<PVUShort>()); break;
        case NDAttrInt32:   valueField->set(PVDC->createPVScalar<PVInt>());    break;
        case NDAttrUInt32:  valueField->set(PVDC->createPVScalar<PVUInt>());   break;
        case NDAttrFloat32: valueField->set(PVDC->createPVScalar<PVFloat>());  break;
        case NDAttrFloat64: valueField->set(PVDC->createPVScalar<PVDouble>()); break;
        case NDAttrString:  valueField->set(PVDC->createPVScalar<PVString>()); break;
        case NDAttrUndefined:
        default:
            throw std::runtime_error("invalid attribute data type");
        }

        destVec.push_back(pvAttr);
        attr = src->next(attr);
    }

    dest->replace(freeze(destVec));
}

static void copyAttributes (PVStructureArrayPtr dest, NDAttributeList *src)
{
    if(dest->view().dataCount() != (size_t)src->count())
        createAttributes(dest, src);

    PVStructureArray::svector destVec(dest->reuse());
    NDAttribute *attr = src->next(NULL);

    for(PVStructureArray::svector::iterator it = destVec.begin();
            it != destVec.end(); ++it)
    {
        PVStructurePtr pvAttr = *it;
        switch(attr->getDataType())
        {
        case NDAttrInt8:    copyAttribute <PVByte,   int8_t>  (pvAttr, attr); break;
        case NDAttrUInt8:   copyAttribute <PVUByte,  uint8_t> (pvAttr, attr); break;
        case NDAttrInt16:   copyAttribute <PVShort,  int16_t> (pvAttr, attr); break;
        case NDAttrUInt16:  copyAttribute <PVUShort, uint16_t>(pvAttr, attr); break;
        case NDAttrInt32:   copyAttribute <PVInt,    int32_t> (pvAttr, attr); break;
        case NDAttrUInt32:  copyAttribute <PVUInt,   uint32_t>(pvAttr, attr); break;
        case NDAttrFloat32: copyAttribute <PVFloat,  float>   (pvAttr, attr); break;
        case NDAttrFloat64: copyAttribute <PVDouble, double>  (pvAttr, attr); break;
        case NDAttrString:  copyStringAttribute(pvAttr, attr); break;
        case NDAttrUndefined:
        default:
            throw std::runtime_error("invalid attribute data type");
        }
        attr = src->next(attr);
    }

    dest->replace(freeze(destVec));
}

static void copyDimension (PVStructureArrayPtr dest, NDDimension_t src[], int ndims)
{
    PVStructureArray::svector destVec(dest->reuse());
    destVec.resize(ndims);
    for (int i = 0; i < ndims; i++)
    {
        if (!destVec[i])
        {
            StructureConstPtr dimStructure = dest->getStructureArray()->getStructure();
            destVec[i] = PVDC->createPVStructure(dimStructure);
        }

        destVec[i]->getSubField<PVInt>("size")->put(src[i].size);
        destVec[i]->getSubField<PVInt>("offset")->put(src[i].offset);
        destVec[i]->getSubField<PVInt>("fullSize")->put(src[i].size);
        destVec[i]->getSubField<PVInt>("binning")->put(src[i].binning);
        destVec[i]->getSubField<PVBoolean>("reverse")->put(src[i].reverse);
    }
    dest->replace(freeze(destVec));
}

static void copyTimeStamp (PVStructurePtr dest, double src)
{
    double seconds = floor(src);
    double nanoseconds = (src - seconds)*1e9;

    PVTimeStamp pvDest;
    pvDest.attach(dest);

    TimeStamp ts((int64_t)seconds, (int32_t)nanoseconds);
    pvDest.set(ts);
}

static void copyTimeStamp (PVStructurePtr dest, epicsTimeStamp src)
{
    PVTimeStamp pvDest;
    pvDest.attach(dest);

    TimeStamp ts(src.secPastEpoch, src.nsec);
    pvDest.set(ts);
}

static void copyToNTNDArray(NTNDArrayPtr ntndarray, NDArray *ndarray)
{
    copyValue(ntndarray, ndarray);

    ntndarray->getCodec()->getSubField<PVString>("name")->put("");

    copyDimension(ntndarray->getDimension(), ndarray->dims, ndarray->ndims);

    ntndarray->getCompressedDataSize()->put(static_cast<int64>(ndarray->dataSize));
    ntndarray->getUncompressedDataSize()->put(static_cast<int64>(ndarray->dataSize));
    ntndarray->getUniqueId()->put(ndarray->uniqueId);

    copyAttributes(ntndarray->getAttribute(), ndarray->pAttributeList);

    copyTimeStamp(ntndarray->getTimeStamp(), ndarray->epicsTS);
    copyTimeStamp(ntndarray->getDataTimeStamp(), ndarray->timeStamp);
}

class epicsShareClass NTNDArrayRecord :
    public PVRecord
{

private:
    NTNDArrayRecord(string const & name, PVStructurePtr const & pvStructure)
    :PVRecord(name, pvStructure) {}

    NTNDArrayPtr m_ntndArray;

public:
    POINTER_DEFINITIONS(NTNDArrayRecord);

    virtual ~NTNDArrayRecord () {}
    static NTNDArrayRecordPtr create (string const & name);
    virtual bool init ();
    virtual void destroy ();
    virtual void process () {}
    void update (NDArray *pArray);
};

NTNDArrayRecordPtr NTNDArrayRecord::create (string const & name)
{
    NTNDArrayBuilderPtr builder = NTNDArray::createBuilder();
    builder->addDescriptor()->addTimeStamp()->addAlarm()->addDisplay();

    NTNDArrayRecordPtr pvRecord(new NTNDArrayRecord(name,
            builder->createPVStructure()));

    if(!pvRecord->init())
        pvRecord.reset();

    return pvRecord;
}

bool NTNDArrayRecord::init ()
{
    initPVRecord();
    m_ntndArray = NTNDArray::wrap(getPVStructure());
    return true;
}

void NTNDArrayRecord::destroy()
{
    PVRecord::destroy();
}

void NTNDArrayRecord::update(NDArray *pArray)
{
    lock();

    try
    {
        beginGroupPut();
        copyToNTNDArray(m_ntndArray, pArray);
        endGroupPut();
    }
    catch(...)
    {
        endGroupPut();
        unlock();
        throw;
    }
    unlock();
}


/** Callback function that is called by the NDArray driver with new NDArray data.
  * \param[in] pArray  The NDArray from the callback.
  */
void NDPluginV4Server::processCallbacks(NDArray *pArray)
{
    NDPluginDriver::processCallbacks(pArray);   // Base class method

    this->unlock();             // Function called with the lock taken
    m_record->update(pArray);
    this->lock();               // Must return locked

    callParamCallbacks();
}

/** Constructor for NDPluginV4Server; all parameters are simply passed to NDPluginDriver::NDPluginDriver.
  * This plugin cannot block (ASYN_CANBLOCK=0) and is not multi-device (ASYN_MULTIDEVICE=0).
  * It has no parameters (0)
  * It allocates a maximum of 2 NDArray buffers for internal use.
  * \param[in] portName The name of the asyn port driver to be created.
  * \param[in] queueSize The number of NDArrays that the input queue for this plugin can hold when
  *            NDPluginDriverBlockingCallbacks=0.  Larger queues can decrease the number of dropped arrays,
  *            at the expense of more NDArray buffers being allocated from the underlying driver's NDArrayPool.
  * \param[in] blockingCallbacks Initial setting for the NDPluginDriverBlockingCallbacks flag.
  *            0=callbacks are queued and executed by the callback thread; 1 callbacks execute in the thread
  *            of the driver doing the callbacks.
  * \param[in] NDArrayPort Name of asyn port driver for initial source of NDArray callbacks.
  * \param[in] NDArrayAddr asyn port driver address for initial source of NDArray callbacks.
  * \param[in] pvName Name of the PV that will be served by the EPICSv4 server.
  * \param[in] maxMemory The maximum amount of memory that the NDArrayPool for this driver is
  *            allowed to allocate. Set this to -1 to allow an unlimited amount of memory.
  * \param[in] priority The thread priority for the asyn port driver thread if ASYN_CANBLOCK is set in asynFlags.
  * \param[in] stackSize The stack size for the asyn port driver thread if ASYN_CANBLOCK is set in asynFlags.
  */
NDPluginV4Server::NDPluginV4Server(const char *portName, int queueSize, int blockingCallbacks,
                                     const char *NDArrayPort, int NDArrayAddr, const char *pvName,
                                     size_t maxMemory, int priority, int stackSize)
    /* Invoke the base class constructor */
    : NDPluginDriver(portName, queueSize, blockingCallbacks,
                   NDArrayPort, NDArrayAddr, 1, 0, 2, maxMemory, 0, 0,
                   /* asynFlags is set to 0, because this plugin cannot block and is not multi-device.
                    * It does autoconnect */
                   0, 1, priority, stackSize),
                   m_record(NTNDArrayRecord::create(pvName))
{
    if(!m_record.get())
        throw runtime_error("failed to create NTNDArrayRecord");

    /* Set the plugin type string */
    setStringParam(NDPluginDriverPluginType, "NDPluginV4Server");

    /* Try to connect to the NDArray port */
    connectToArrayPort();

    PVDatabasePtr master = PVDatabase::getMaster();
    ChannelProviderLocalPtr channelProvider = getChannelProviderLocal();

    if(!master->addRecord(m_record))
        throw runtime_error("couldn't add record to master database");

    m_server = startPVAServer(PVACCESS_ALL_PROVIDERS, 0, true, true);
}

/* Configuration routine.  Called directly, or from the iocsh function */
extern "C" int NDV4ServerConfigure(const char *portName, int queueSize, int blockingCallbacks,
                                    const char *NDArrayPort, int NDArrayAddr, const char *pvName,
                                    size_t maxMemory, int priority, int stackSize)
{
    new NDPluginV4Server(portName, queueSize, blockingCallbacks, NDArrayPort, NDArrayAddr, pvName,
                          maxMemory, priority, stackSize);
    return(asynSuccess);
}

/* EPICS iocsh shell commands */
static const iocshArg initArg0 = { "portName",iocshArgString};
static const iocshArg initArg1 = { "frame queue size",iocshArgInt};
static const iocshArg initArg2 = { "blocking callbacks",iocshArgInt};
static const iocshArg initArg3 = { "NDArrayPort",iocshArgString};
static const iocshArg initArg4 = { "NDArrayAddr",iocshArgInt};
static const iocshArg initArg5 = { "pvName",iocshArgString};
static const iocshArg initArg6 = { "maxMemory",iocshArgInt};
static const iocshArg initArg7 = { "priority",iocshArgInt};
static const iocshArg initArg8 = { "stack size",iocshArgInt};
static const iocshArg * const initArgs[] = {&initArg0,
                                            &initArg1,
                                            &initArg2,
                                            &initArg3,
                                            &initArg4,
                                            &initArg5,
                                            &initArg6,
                                            &initArg7,
                                            &initArg8,};
static const iocshFuncDef initFuncDef = {"NDV4ServerConfigure",9,initArgs};
static void initCallFunc(const iocshArgBuf *args)
{
    NDV4ServerConfigure(args[0].sval, args[1].ival, args[2].ival,
                         args[3].sval, args[4].ival, args[5].sval,
                         args[6].ival, args[7].ival, args[8].ival);
}

extern "C" void NDV4ServerRegister(void)
{
    iocshRegister(&initFuncDef,initCallFunc);
}

extern "C" {
epicsExportRegistrar(NDV4ServerRegister);
}
