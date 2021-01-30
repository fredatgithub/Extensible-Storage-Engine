// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "std.hxx"
#include "_dump.hxx"

const RBS_POS rbsposMin = { 0x0,  0x0 };

ERR ErrBeginDatabaseIncReseedTracing( _In_ IFileSystemAPI* pfsapi, _In_ JET_PCWSTR wszDatabase, _Out_ CPRINTF** ppcprintf );
ERR ErrDBFormatFeatureEnabled_( const JET_ENGINEFORMATVERSION efvFormatFeature, const DbVersion& dbvCurrentFromFile );
VOID TraceFuncBegun( CPRINTF* const pcprintf, const CHAR* const szFunction );
VOID TraceFuncComplete( CPRINTF* const pcprintf, const CHAR* const szFunction, const ERR err );

#ifdef ENABLE_JET_UNIT_TEST
JETUNITTEST( CmpRbspos, Test )
{
    RBS_POS pos1 = { 10, 10 }, pos2 = { 20, 10 }, pos3 = { 10, 20 }, pos4 = { 30, 20 };
    CHECK( CmpRbspos( pos1, pos1 ) == 0 );
    CHECK( CmpRbspos( pos1, pos2 ) < 0 );
    CHECK( CmpRbspos( pos2, pos1 ) > 0 );
    CHECK( CmpRbspos( pos2, pos3 ) < 0 );
    CHECK( CmpRbspos( pos3, pos2 ) > 0 );
    CHECK( CmpRbspos( pos1, pos3 ) < 0 );
    CHECK( CmpRbspos( pos3, pos1 ) > 0 );
    CHECK( CmpRbspos( pos1, pos4 ) < 0 );
    CHECK( CmpRbspos( pos4, pos1 ) > 0 );
}
#endif

LOCAL ERR ErrAllocAndSetStr( __in PCWSTR wszName, __out WCHAR** pwszResult )
{
    Assert( wszName );
    ERR err = JET_errSuccess;
    LONG cbDest =  ( LOSStrLengthW( wszName ) + 1 ) * sizeof(WCHAR);

    if ( *pwszResult )
    {
        OSMemoryPageFree( *pwszResult );
    }

    *pwszResult = static_cast<WCHAR *>( PvOSMemoryHeapAlloc( cbDest ) );
    Alloc( *pwszResult );

    Call( ErrOSStrCbCopyW( *pwszResult, cbDest, wszName ) );
HandleError:
    return err;
}

LOCAL ERR ErrRBSAbsRootDir( INST* pinst, __out_bcount( cbRBSRootDir ) PWSTR wszRBSRootDirPath, LONG cbRBSRootDir )
{
    ERR err                 = JET_errSuccess;
    PCWSTR wszRBSFilePath   = FDefaultParam( pinst, JET_paramRBSFilePath ) ? SzParam( pinst, JET_paramLogFilePath ) : SzParam( pinst, JET_paramRBSFilePath );
    WCHAR wszAbsDirRootPath[ IFileSystemAPI::cchPathMax ];

    if ( NULL == wszRBSFilePath || 0 == *wszRBSFilePath )
    {
        return ErrERRCheck(JET_errInvalidParameter);
    }

    Call( pinst->m_pfsapi->ErrPathComplete( wszRBSFilePath, wszAbsDirRootPath ) );
    Call( pinst->m_pfsapi->ErrPathFolderNorm( wszAbsDirRootPath, sizeof( wszAbsDirRootPath ) ) );
    Call( ErrOSStrCbAppendW( wszAbsDirRootPath, sizeof( wszAbsDirRootPath ), wszRBSDirRoot ) );
    Call( pinst->m_pfsapi->ErrPathFolderNorm( wszAbsDirRootPath, sizeof( wszAbsDirRootPath ) ) );

    Assert( cbRBSRootDir > LOSStrLengthW( wszAbsDirRootPath ) * sizeof(WCHAR) ); 

    Call( ErrOSStrCbCopyW( wszRBSRootDirPath, cbRBSRootDir, wszAbsDirRootPath ) );

HandleError:
    return err;
}

LOCAL ERR ErrRBSInitPaths_( INST* pinst, __out WCHAR** wszRBSAbsRootDir, __out WCHAR** wszRBSBaseName )
{
    Assert( pinst );
    Assert( wszRBSAbsRootDir );

    WCHAR   wszAbsDirRootPath[ IFileSystemAPI::cchPathMax ];
    PCWSTR  wszBaseName     = SzParam( pinst, JET_paramBaseName );
    ERR     err             = JET_errSuccess;

    if ( NULL == wszBaseName || 0 == *wszBaseName )
    {
        return ErrERRCheck(JET_errInvalidParameter);
    }

    Call( ErrRBSAbsRootDir( pinst, wszAbsDirRootPath, sizeof( wszAbsDirRootPath ) ) );
    Call( ErrAllocAndSetStr( wszAbsDirRootPath, wszRBSAbsRootDir ) );
    Call( ErrAllocAndSetStr( wszBaseName, wszRBSBaseName ) );

HandleError:
    return err;
}

LOCAL BOOL FRBSCheckForDbConsistency(
    const SIGNATURE* const psignDbHdrFlushFromDb,
    const SIGNATURE* const psigRBSHdrFlushFromDb,
    const SIGNATURE* const psignDbHdrFlushFromRBS,
    const SIGNATURE* const psignRBSHdrFlushFromRBS )
{
    return ( ( FSIGSignSet( psignDbHdrFlushFromDb ) && 
               memcmp( psignDbHdrFlushFromDb, psignDbHdrFlushFromRBS, sizeof( SIGNATURE ) ) == 0 ) ||
             ( FSIGSignSet( psigRBSHdrFlushFromDb ) &&
               memcmp( psigRBSHdrFlushFromDb, psignRBSHdrFlushFromRBS, sizeof( SIGNATURE ) ) == 0 ) );
}

LOCAL ERR ErrRBSGetDirSize( IFileSystemAPI *pfsapi, PCWSTR wszDirPath, _Out_ QWORD* pcbSize )
{
    ERR err;
    IFileFindAPI    *pffapi = NULL;
    QWORD dirSize = 0;
    *pcbSize = 0;

    WCHAR wszPath[ IFileSystemAPI::cchPathMax ];
    Call( ErrOSStrCbCopyW( wszPath, sizeof( wszPath ), wszDirPath ) );
    Call( pfsapi->ErrPathFolderNorm( wszPath, sizeof( wszPath ) ) );
    Call( ErrOSStrCbAppendW( wszPath, sizeof( wszPath ), L"*" ) );

    Call( pfsapi->ErrFileFind( wszPath, &pffapi ) );

    while ( ( err = pffapi->ErrNext() ) == JET_errSuccess )
    {
        WCHAR wszFileName[IFileSystemAPI::cchPathMax];
        WCHAR wszDirT[IFileSystemAPI::cchPathMax];
        WCHAR wszFileT[IFileSystemAPI::cchPathMax];
        WCHAR wszExtT[IFileSystemAPI::cchPathMax];
        WCHAR wszFileNameT[IFileSystemAPI::cchPathMax];
        QWORD cbSize;
        BOOL fFolder;

        Call( pffapi->ErrIsFolder( &fFolder ) );

        Call( pffapi->ErrPath( wszFileName ) );
        Call( pfsapi->ErrPathParse( wszFileName, wszDirT, wszFileT, wszExtT ) );
        wszDirT[0] = 0;
        Call( pfsapi->ErrPathBuild( wszDirT, wszFileT, wszExtT, wszFileNameT ) );

         if ( wcscmp( wszFileNameT, L"." ) == 0 ||
             wcscmp( wszFileNameT, L".." ) == 0 )
         {
             continue;
         }

        if ( fFolder )
        {
            Call( ErrRBSGetDirSize ( pfsapi, wszFileName, &cbSize ) );
            dirSize += cbSize;
        }
        else
        {
            Call( pffapi->ErrSize( &cbSize, IFileAPI::filesizeOnDisk ) );
            dirSize += cbSize;
        }
    }

    Call( err == JET_errFileNotFound ? JET_errSuccess : err );

    err = JET_errSuccess;
    *pcbSize = dirSize;

HandleError:

    if ( pffapi != NULL )
    {
        delete pffapi;
    }

    return err;
}


LOCAL ERR ErrRBSDeleteAllFiles( IFileSystemAPI *const pfsapi, PCWSTR wszDir, PCWSTR wszFilter, BOOL fRecursive )
{
    ERR             err     = JET_errSuccess;
    IFileFindAPI*   pffapi  = NULL;

    Assert( LOSStrLengthW( wszDir ) + 1 + 1 < IFileSystemAPI::cchPathMax );


    WCHAR wszPath[ IFileSystemAPI::cchPathMax ];
    Call( ErrOSStrCbCopyW( wszPath, sizeof( wszPath ), wszDir ) );
    Call( pfsapi->ErrPathFolderNorm( wszPath, sizeof( wszPath ) ) );

    if ( !wszFilter )
    {
        Call( ErrOSStrCbAppendW( wszPath, sizeof( wszPath ), L"*" ) );
    }
    else
    {
        Call( ErrOSStrCbAppendW( wszPath, sizeof( wszPath ), wszFilter ) );
    }

    Call( pfsapi->ErrFileFind( wszPath, &pffapi ) );
    while ( ( err = pffapi->ErrNext() ) == JET_errSuccess )
    {
        WCHAR wszFileName[IFileSystemAPI::cchPathMax];
        WCHAR wszDirT[IFileSystemAPI::cchPathMax];
        WCHAR wszFileT[IFileSystemAPI::cchPathMax];
        WCHAR wszExtT[IFileSystemAPI::cchPathMax];
        WCHAR wszFileNameT[IFileSystemAPI::cchPathMax];
        BOOL fFolder;

        Call( pffapi->ErrIsFolder( &fFolder ) );

        Call( pffapi->ErrPath( wszFileName ) );
        Call( pfsapi->ErrPathParse( wszFileName, wszDirT, wszFileT, wszExtT ) );
        wszDirT[0] = 0;
        Call( pfsapi->ErrPathBuild( wszDirT, wszFileT, wszExtT, wszFileNameT ) );

        
        if (    wcscmp( wszFileNameT, L"." ) != 0 &&
                wcscmp( wszFileNameT, L".." ) != 0 )
        {
            if ( !fFolder )
            {
                Call( pfsapi->ErrFileDelete( wszFileName ) );
            }
            else if ( fRecursive )
            {
                Call( ErrRBSDeleteAllFiles( pfsapi, wszFileName, wszFilter, fRecursive ) );
            }
        }
    }

    if ( fRecursive )
    {
        Call( pfsapi->ErrFolderRemove( wszDir ) );
    }

    Call( err == JET_errFileNotFound ? JET_errSuccess : err );

    err = JET_errSuccess;

HandleError:
    
    Assert( wszDir[LOSStrLengthW(wszDir)] != L'*' );

    if ( pffapi != NULL )
    {
        delete pffapi;
    }

    return err;
}

LOCAL ERR ErrRBSDirPrefix(
    PCWSTR wszLogBaseName,
    __out_bcount( cbDirPrefix ) PWSTR wszRBSDirPrefix,
    LONG cbDirPrefix )
{
    Assert( wszRBSDirPrefix );
    Assert( cbDirPrefix > ( LOSStrLengthW( wszRBSDirBase ) + LOSStrLengthW( wszLogBaseName ) + 1 ) * sizeof(WCHAR) );

    ERR err = JET_errSuccess;
    Call( ErrOSStrCbCopyW( wszRBSDirPrefix, cbDirPrefix, wszRBSDirBase ) );

    Call( ErrOSStrCbAppendW( wszRBSDirPrefix, cbDirPrefix, wszLogBaseName ) );

HandleError:
    return err;
}

LOCAL ERR ErrRBSGetLowestAndHighestGen_( 
    IFileSystemAPI *const pfsapi, 
    __in const PCWSTR wszRBSDirRootPath, 
    __in const PCWSTR wszLogBaseName,  
    LONG *rbsGenMin,
    LONG *rbsGenMax )
{
    WCHAR       wszRBSDirPrefix[ IFileSystemAPI::cchPathMax ];
    ERR             err                 = JET_errSuccess;
    IFileFindAPI*   pffapi              = NULL;
    LONG            lRBSGen                = 0;
    ULONG           cCurrentRBSDigits   = 0;

    *rbsGenMin = 0;
    *rbsGenMax = 0;

    Assert( LOSStrLengthW( wszRBSDirRootPath ) + 1 + 1 < IFileSystemAPI::cchPathMax );

    WCHAR wszSearchPath[ IFileSystemAPI::cchPathMax ];
    Call( ErrOSStrCbCopyW( wszSearchPath, sizeof( wszSearchPath ), wszRBSDirRootPath ) );
    Call( pfsapi->ErrPathFolderNorm( wszSearchPath, sizeof( wszSearchPath ) ) );

    Call( ErrRBSDirPrefix( wszLogBaseName, wszRBSDirPrefix, sizeof( wszRBSDirPrefix ) ) );

    Call( ErrOSStrCbAppendW( wszSearchPath, sizeof( wszSearchPath ), wszRBSDirPrefix ) );
    Call( ErrOSStrCbAppendW( wszSearchPath, sizeof( wszSearchPath ), L"*" ) );

    Call( pfsapi->ErrFileFind( wszSearchPath, &pffapi ) );
    while ( ( err = pffapi->ErrNext() ) == JET_errSuccess )
    {
        WCHAR wszFileName[IFileSystemAPI::cchPathMax];
        WCHAR wszDirT[IFileSystemAPI::cchPathMax];
        WCHAR wszFileT[IFileSystemAPI::cchPathMax];
        WCHAR wszExtT[IFileSystemAPI::cchPathMax];

        Call( pffapi->ErrPath( wszFileName ) );
        Call( pfsapi->ErrPathParse( wszFileName, wszDirT, wszFileT, wszExtT ) );

        if ( 0 == LOSStrLengthW( wszExtT ) )
        {
            err = LGFileHelper::ErrLGGetGeneration( pfsapi, wszFileT, wszRBSDirPrefix, &lRBSGen, wszExtT, &cCurrentRBSDigits);

            if ( lRBSGen < *rbsGenMin || *rbsGenMin == 0 )
            {
                *rbsGenMin = lRBSGen;
            }

            if ( lRBSGen > *rbsGenMax || *rbsGenMax == 0 )
            {
                *rbsGenMax = lRBSGen;
            }
        }
    }
    Call( err == JET_errFileNotFound ? JET_errSuccess : err );

    err = JET_errSuccess;

HandleError:

    delete pffapi;

    return err;
}

const WCHAR rgwchRBSFileDigits[] =
{
    L'0', L'1', L'2', L'3', L'4', L'5', L'6', L'7',
    L'8', L'9', L'A', L'B', L'C', L'D', L'E', L'F',
};
const LONG lRBSFileBase = sizeof( rgwchRBSFileDigits )/sizeof( rgwchRBSFileDigits[0] );
const LONG cchRBSDigits = 8;

LOCAL VOID RBSSzIdAppend( __inout_bcount_z( cbFName ) PWSTR wszRBSFileName, size_t cbFName, LONG rbsGeneration )
{
    LONG    ich;
    LONG    ichBase;

    Assert( wszRBSFileName );

    ichBase = wcslen( wszRBSFileName );

    Assert( cbFName >= ( ( ichBase+cchRBSDigits+1 )*sizeof(WCHAR) ) );

    ich = cchRBSDigits + ichBase - 1;

    if ( ( sizeof(WCHAR)*( cchRBSDigits+ichBase+1 ) ) > cbFName )
    {
        AssertSz(false, "Someone passed in a path too short for RBSSzIdAppend(), we'll recover, though I don't know how well");
        if ( cbFName >= sizeof(WCHAR) )
        {
            OSStrCbAppendW( wszRBSFileName, cbFName, L"" );
        }
        return;
    }

    for ( ; ich > (ichBase-1); ich-- )
    {
        wszRBSFileName[ich] = rgwchRBSFileDigits[rbsGeneration % lRBSFileBase];
        rbsGeneration = rbsGeneration / lRBSFileBase;
    }
    Assert( rbsGeneration == 0 );
    wszRBSFileName[ichBase+cchRBSDigits] = 0;
}

LOCAL ERR ErrRBSFilePathForGen_( 
    PCWSTR cwszPath,
    PCWSTR cwszRBSBaseName,
    __in IFileSystemAPI* const pfsapi,
    __out_bcount ( cbDirPath ) WCHAR* wszRBSDirPath,
    LONG cbDirPath,
    __out_bcount ( cbFilePath ) WCHAR* wszRBSFilePath,
    LONG cbFilePath,
    LONG lRBSGen )
{
    WCHAR       wszFileName[ IFileSystemAPI::cchPathMax ];
    ERR         err     = JET_errSuccess;
    
    Assert ( cwszPath );
    Assert ( wszRBSDirBase );
    Assert ( wszRBSFilePath );
    Assert ( wszRBSDirPath );

    Assert ( ( LOSStrLengthW( cwszPath ) + LOSStrLengthW( wszRBSDirBase ) + LOSStrLengthW( cwszRBSBaseName ) + cchRBSDigits + 2 + 1) * sizeof(WCHAR) < cbDirPath );

    Call( ErrOSStrCbCopyW( wszRBSDirPath, cbDirPath, cwszPath ) );
    Call( pfsapi->ErrPathFolderNorm( wszRBSDirPath, cbDirPath ) );

    Call( ErrOSStrCbAppendW( wszRBSDirPath, cbDirPath, wszRBSDirBase ) );
    Call( ErrOSStrCbAppendW( wszRBSDirPath, cbDirPath, cwszRBSBaseName ) );

    RBSSzIdAppend ( wszRBSDirPath, cbDirPath, lRBSGen );

    Call( pfsapi->ErrPathFolderNorm( wszRBSDirPath, cbDirPath ) );

    Assert ( sizeof( wszFileName ) > ( LOSStrLengthW( cwszRBSBaseName ) + cchRBSDigits + LOSStrLengthW( wszRBSExt ) + 1 ) * sizeof(WCHAR) ); 
    Call( ErrOSStrCbCopyW( wszFileName, sizeof( wszFileName ), cwszRBSBaseName ) );
    
    RBSSzIdAppend ( wszFileName, sizeof( wszFileName ), lRBSGen );

    Assert ( cbFilePath > ( LOSStrLengthW( wszRBSDirPath ) + LOSStrLengthW( wszFileName ) + LOSStrLengthW( wszRBSExt ) + 1 ) * sizeof(WCHAR) );

    Call( pfsapi->ErrPathBuild( wszRBSDirPath, wszFileName, wszRBSExt, wszRBSFilePath ) );

HandleError:
    return err;
}

LOCAL VOID RBSInitFileHdr(
    _In_ const LONG                         lRBSGen,
    _Out_ RBSFILEHDR * const                prbsfilehdr,
    _In_ const LOGTIME                      tmPrevGen )
{
    prbsfilehdr->rbsfilehdr.le_lGeneration          = lRBSGen;
    LGIGetDateTime( &prbsfilehdr->rbsfilehdr.tmCreate );
    prbsfilehdr->rbsfilehdr.tmPrevGen = tmPrevGen;
    prbsfilehdr->rbsfilehdr.le_ulMajor              = ulRBSVersionMajor;
    prbsfilehdr->rbsfilehdr.le_ulMinor              = ulRBSVersionMinor;
    prbsfilehdr->rbsfilehdr.le_filetype             = JET_filetypeSnapshot;
    prbsfilehdr->rbsfilehdr.le_cbLogicalFileSize    = QWORD( 2 * sizeof( RBSFILEHDR ) );
    prbsfilehdr->rbsfilehdr.le_cbDbPageSize         = (USHORT)g_cbPage;

    SIGGetSignature( &prbsfilehdr->rbsfilehdr.signRBSHdrFlush );
}

LOCAL ERR ErrRBSInitAttachInfo(
    _In_ RBSATTACHINFO* prbsattachinfo,
    _In_ PCWSTR wszDatabaseName,
    LONG lGenMinRequired,
    LONG lGenMaxRequired,
    _In_ DBTIME dbtimePrevDirtied,
    SIGNATURE signDb, 
    SIGNATURE signDbHdrFlush )
{
    Assert( prbsattachinfo );

    prbsattachinfo->SetPresent( 1 );
    prbsattachinfo->SetDbtimeDirtied( 0 );
    prbsattachinfo->SetDbtimePrevDirtied( dbtimePrevDirtied );
    prbsattachinfo->SetLGenMinRequired( lGenMinRequired );
    prbsattachinfo->SetLGenMaxRequired( lGenMaxRequired );

    UtilMemCpy( &prbsattachinfo->signDb, &signDb, sizeof( SIGNATURE ) );
    UtilMemCpy( &prbsattachinfo->signDbHdrFlush, &signDbHdrFlush, sizeof( SIGNATURE ) );

    DWORD cbDatabaseName = (wcslen(wszDatabaseName) + 1)*sizeof(WCHAR);
    if ( cbDatabaseName > sizeof(prbsattachinfo->wszDatabaseName) )
    {
        return ErrERRCheck( JET_errBufferTooSmall );
    }
    UtilMemCpy( prbsattachinfo->wszDatabaseName, wszDatabaseName, cbDatabaseName );
    return JET_errSuccess;
}

LOCAL VOID RBSLoadRequiredGenerationFromFMP( INST* pinst, _Out_ LONG* plgenLow, _Out_ LONG* plgenHigh )
{
    LONG lgenLow        = lMax;
    LONG lgenHigh       = lMin;
    BOOL fDatabaseFound = fFalse;

    *plgenLow = 0;
    *plgenHigh = 0;

    for ( DBID dbid = dbidUserLeast; dbid < dbidMax; ++dbid )
    {
        IFMP        ifmp    = pinst->m_mpdbidifmp[ dbid ];
        if ( ifmp >= g_ifmpMax )
            continue;

        FMP         *pfmp   = &g_rgfmp[ifmp];
        PdbfilehdrReadOnly pdbfilehdr    = pfmp->Pdbfilehdr();
        if ( pdbfilehdr == NULL || ( pdbfilehdr->le_dbstate != JET_dbstateDirtyShutdown && pdbfilehdr->le_dbstate != JET_dbstateDirtyAndPatchedShutdown ) )
            continue;

        fDatabaseFound      = fTrue;

        Assert( pdbfilehdr.get() );

        if ( pdbfilehdr->le_lGenMinRequired < lgenLow )
        {
            lgenLow = min( lgenLow, pdbfilehdr->le_lGenMinRequired );
        }

        if ( pdbfilehdr->le_lGenMaxRequired > lgenHigh )
        {
            lgenHigh = max( lgenHigh, pdbfilehdr->le_lGenMaxRequired );
        }
    }

    if ( fDatabaseFound )
    {
        *plgenLow = lgenLow;
        *plgenHigh = lgenHigh;
    }
}

LOCAL ERR ErrRBSCopyRequiredLogs_( 
    INST* pinst, 
    long lLogGenMinReq, 
    long lLogGenMaxReq, 
    PCWSTR wszSrcDir, 
    PCWSTR wszDestDir, 
    BOOL fOverwriteExisting,
    BOOL fCopyCurrentIfMaxMissing )
{
    Assert( pinst != NULL );
    Assert( pinst->m_pfsapi != NULL );
    Assert( lLogGenMaxReq >= lLogGenMinReq );
    Assert( wszDestDir != NULL );
    Assert( wszSrcDir != NULL );

    WCHAR wszDestLogFilePath[IFileSystemAPI::cchPathMax];
    WCHAR wszLogFilePath[IFileSystemAPI::cchPathMax];
    WCHAR wszDirT[IFileSystemAPI::cchPathMax];
    WCHAR wszFileT[IFileSystemAPI::cchPathMax];
    WCHAR wszExtT[IFileSystemAPI::cchPathMax];

    IFileSystemAPI* pfsapi  = pinst->m_pfsapi;
    ERR             err     = JET_errSuccess;

    Assert( lLogGenMinReq > 0 || lLogGenMinReq == lLogGenMaxReq );

    for (long lGenToCopy = lLogGenMinReq; lGenToCopy <= lLogGenMaxReq && lGenToCopy > 0; ++lGenToCopy )
    {
        wszDestLogFilePath[0] = 0;
        wszFileT[0] = 0;
        wszExtT[0] = 0;

        Call( LGFileHelper::ErrLGMakeLogNameBaselessEx( 
            wszLogFilePath,
            sizeof( wszLogFilePath ),
            pfsapi,
            wszSrcDir,
            SzParam( pinst, JET_paramBaseName ),
            eArchiveLog,
            lGenToCopy,
            ( UlParam( pinst, JET_paramLegacyFileNames ) & JET_bitESE98FileNames ) ? wszOldLogExt : wszNewLogExt,
            ( UlParam( pinst, JET_paramLegacyFileNames ) & JET_bitEightDotThreeSoftCompat ) ? 0 : 8 ) );       

        if ( lGenToCopy == lLogGenMaxReq &&
            ErrUtilPathExists( pfsapi, wszLogFilePath ) == JET_errFileNotFound )
        {
            if ( !fCopyCurrentIfMaxMissing )
            {
                continue;
            }

            Call( LGFileHelper::ErrLGMakeLogNameBaselessEx( 
                wszLogFilePath,
                sizeof( wszLogFilePath ),
                pfsapi,
                wszSrcDir,
                SzParam( pinst, JET_paramBaseName ),
                eCurrentLog,
                0,
                ( UlParam( pinst, JET_paramLegacyFileNames ) & JET_bitESE98FileNames ) ? wszOldLogExt : wszNewLogExt,
                ( UlParam( pinst, JET_paramLegacyFileNames ) & JET_bitEightDotThreeSoftCompat ) ? 0 : 8 ) );
        }

        Call( pfsapi->ErrPathParse( wszLogFilePath, wszDirT, wszFileT, wszExtT ) );
        Call( pfsapi->ErrPathBuild( wszDestDir, wszFileT, wszExtT, wszDestLogFilePath ) );

        err = pfsapi->ErrFileCopy( wszLogFilePath, wszDestLogFilePath, fOverwriteExisting );
        Call( err == JET_errFileAlreadyExists ? JET_errSuccess : err );
    }

HandleError:
    return err;
}

LOCAL VOID RBSLogCreateSkippedEvent( INST* pinst, PCWSTR wszRBSFilePath, ERR errSkip, ERR errLoad )
{
    WCHAR wszErrSkip[16], wszErrLoad[16];
    OSStrCbFormatW( wszErrSkip, sizeof(wszErrSkip), L"%d", errSkip );
    OSStrCbFormatW( wszErrLoad, sizeof(wszErrLoad), L"%d", errLoad );

    PCWSTR rgcwsz[3];
    rgcwsz[0] = wszRBSFilePath;
    rgcwsz[1] = wszErrSkip;
    rgcwsz[2] = wszErrLoad;

    UtilReportEvent(
        eventError,
        GENERAL_CATEGORY,
        RBS_CREATE_SKIPPED_ID,
        3,
        rgcwsz,
        0,
        NULL,
        pinst );
}

LOCAL VOID RBSLogCreateOrLoadEvent( INST* pinst, PCWSTR wszRBSFilePath, BOOL fNewlyCreated, BOOL fSuccess, ERR err )
{
    WCHAR wszCreatedOrLoaded[16], wszErr[16];
    OSStrCbFormatW( wszCreatedOrLoaded, sizeof(wszCreatedOrLoaded),  L"%ws", fNewlyCreated ? L"created" : L"loaded" );
    OSStrCbFormatW( wszErr, sizeof(wszErr), L"%d", err );

    PCWSTR rgcwsz[3];
    rgcwsz[0] = wszRBSFilePath;
    rgcwsz[1] = wszCreatedOrLoaded;
    rgcwsz[2] = wszErr;

    UtilReportEvent(
            fSuccess ? eventInformation : eventError,
            GENERAL_CATEGORY,
            fSuccess ? RBS_CREATEORLOAD_SUCCESS_ID : RBS_CREATEORLOAD_FAILED_ID,
            3,
            rgcwsz,
            0,
            NULL,
            pinst );
}

LOCAL ERR ErrRBSLoadRbsGen(
    INST*               pinst,
    PWSTR               wszRBSAbsFilePath,
    LONG                lRBSGen,
    _Out_ RBSFILEHDR *  prbshdr, 
    _Out_ IFileAPI**    ppfapiRBS )
{
    Assert( ppfapiRBS );
    Assert( wszRBSAbsFilePath );

    ERR err = JET_errSuccess;

    Call( CIOFilePerf::ErrFileOpen( 
            pinst->m_pfsapi,
            pinst,
            wszRBSAbsFilePath,
            BoolParam( pinst, JET_paramUseFlushForWriteDurability ) ? IFileAPI::fmfStorageWriteBack : IFileAPI::fmfRegular,
            iofileRBS,
            QwInstFileID( qwRBSFileID, pinst->m_iInstance, lRBSGen ),
            ppfapiRBS ) );

    Call( ErrUtilReadShadowedHeader( pinst, pinst->m_pfsapi, *ppfapiRBS, (BYTE*) prbshdr, sizeof( RBSFILEHDR ), -1, urhfNoAutoDetectPageSize ) );

    if (  JET_filetypeSnapshot != prbshdr->rbsfilehdr.le_filetype )
    {
        Call( ErrERRCheck( JET_errFileInvalidType ) );
    }

    if ( prbshdr->rbsfilehdr.le_ulMajor != ulRBSVersionMajor || prbshdr->rbsfilehdr.le_ulMinor > ulRBSVersionMinor )
    {
        Call( ErrERRCheck( JET_errBadRBSVersion ) );
    }

    if ( !FSIGSignSet( &prbshdr->rbsfilehdr.signRBSHdrFlush ) )
    {
        Call( ErrERRCheck( JET_errRBSInvalidSign ) );
    }

HandleError:
    return err;
}

LOCAL ERR ErrRBSPerformLogChecks( 
    INST* pinst, 
    PCWSTR wszRBSAbsRootDirPath,
    PCWSTR wszRBSBaseName,
    LONG lRBSGen,
    BOOL fPerformDivergenceCheck,
    RBSFILEHDR* prbsfilehdr,
    _Out_bytecap_c_(cbOSFSAPI_MAX_PATHW) PWSTR wszRBSAbsLogDirPath )
{
    Assert( pinst );
    Assert( wszRBSAbsRootDirPath );
    Assert( wszRBSBaseName );

    WCHAR wszRBSAbsDirPath[ IFileSystemAPI::cchPathMax ];
    WCHAR wszRBSAbsFilePath[ IFileSystemAPI::cchPathMax ];
    WCHAR wszRBSLogGenPath[ IFileSystemAPI::cchPathMax ];
    WCHAR wszMaxReqLogPathActual[ IFileSystemAPI::cchPathMax ];    

    PCWSTR  wszLogExt       = ( UlParam( pinst, JET_paramLegacyFileNames ) & JET_bitESE98FileNames ) ? wszOldLogExt : wszNewLogExt;
    ULONG   cLogDigits      = ( UlParam( pinst, JET_paramLegacyFileNames ) & JET_bitEightDotThreeSoftCompat )  ? 0 : 8;
    PCWSTR  wszBaseName     = SzParam( pinst, JET_paramBaseName );
    BOOL    fLogsDiverged   = fFalse;
    ERR     err             = JET_errSuccess;
    IFileAPI* pfapirbs      = NULL;

    IFileSystemAPI* pfsapi = pinst->m_pfsapi;
    Assert( pfsapi );

    Call( ErrRBSFilePathForGen_( wszRBSAbsRootDirPath, wszRBSBaseName, pinst->m_pfsapi, wszRBSAbsDirPath, sizeof( wszRBSAbsDirPath ), wszRBSAbsFilePath, cbOSFSAPI_MAX_PATHW, lRBSGen ) );
    Call( CIOFilePerf::ErrFileOpen( pinst->m_pfsapi, pinst, wszRBSAbsFilePath, IFileAPI::fmfReadOnly, iofileRBS, qwRBSFileID, &pfapirbs ) );

    Call( ErrUtilReadShadowedHeader( pinst, pinst->m_pfsapi, pfapirbs, (BYTE*) prbsfilehdr, sizeof( RBSFILEHDR ), -1, urhfNoAutoDetectPageSize | urhfReadOnly | urhfNoEventLogging ) );
    
    Assert( pinst->m_plog );
    Assert( prbsfilehdr->rbsfilehdr.le_lGenMaxLogCopied >= prbsfilehdr->rbsfilehdr.le_lGenMinLogCopied );

    if ( prbsfilehdr->rbsfilehdr.bLogsCopied != 1 )
    {
        Error( ErrERRCheck( JET_errRBSMissingReqLogs ) );
    }

    Assert( LOSStrLengthW( wszRBSAbsDirPath ) + LOSStrLengthW( wszRBSLogDir ) < IFileSystemAPI::cchPathMax );

    Call( ErrOSStrCbCopyW( wszRBSAbsLogDirPath, cbOSFSAPI_MAX_PATHW, wszRBSAbsDirPath ) );
    Call( ErrOSStrCbAppendW( wszRBSAbsLogDirPath, cbOSFSAPI_MAX_PATHW, wszRBSLogDir ) );

    for ( LONG lgen = prbsfilehdr->rbsfilehdr.le_lGenMaxLogCopied; lgen >= prbsfilehdr->rbsfilehdr.le_lGenMinLogCopied && lgen > 0; --lgen )
    {
        Call( LGFileHelper::ErrLGMakeLogNameBaselessEx(
            wszRBSLogGenPath,
            sizeof( wszRBSLogGenPath ),
            pfsapi,
            wszRBSAbsLogDirPath,
            wszBaseName,
            eArchiveLog,
            lgen,
            wszLogExt,
            cLogDigits ) );

        err = ErrUtilPathExists( pfsapi, wszRBSLogGenPath );

        if ( err == JET_errFileNotFound )
        {
            if ( lgen == prbsfilehdr->rbsfilehdr.le_lGenMaxLogCopied )
            {
                Call( LGFileHelper::ErrLGMakeLogNameBaselessEx(
                    wszRBSLogGenPath,
                    sizeof( wszRBSLogGenPath ),
                    pfsapi,
                    wszRBSAbsLogDirPath,
                    wszBaseName,
                    eCurrentLog,
                    0,
                    wszLogExt,
                    cLogDigits ) );

                err = ErrUtilPathExists( pfsapi, wszRBSLogGenPath );
            }

            if ( err == JET_errFileNotFound )
            {
                Error( ErrERRCheck( JET_errRBSMissingReqLogs ) );
            }
        }

        Call( err );

        if ( fPerformDivergenceCheck && lgen == prbsfilehdr->rbsfilehdr.le_lGenMaxLogCopied )
        {
            Call( LGFileHelper::ErrLGMakeLogNameBaselessEx(
                wszMaxReqLogPathActual,
                sizeof( wszMaxReqLogPathActual ),
                pfsapi,
                SzParam( pinst, JET_paramLogFilePath ),
                SzParam( pinst, JET_paramBaseName ),
                eArchiveLog,
                lgen,
                ( UlParam( pinst, JET_paramLegacyFileNames ) & JET_bitESE98FileNames ) ? wszOldLogExt : wszNewLogExt,
                ( UlParam( pinst, JET_paramLegacyFileNames ) & JET_bitEightDotThreeSoftCompat ) ? 0 : 8 ) );

            err = ErrUtilPathExists( pfsapi, wszMaxReqLogPathActual );

            if ( err == JET_errFileNotFound )
            {
                Error( ErrERRCheck( JET_errRBSCannotDetermineDivergence ) );
            }

            Call( err );
            Assert( pinst->m_plog );
            Call( pinst->m_plog->ErrCompareLogs( wszRBSLogGenPath, wszMaxReqLogPathActual, fTrue, &fLogsDiverged ) );

            if ( fLogsDiverged )
            {
                Error( ErrERRCheck( JET_errRBSLogDivergenceFailed ) );
            }
        }
    }

HandleError:
    if ( pfapirbs )
    {
        delete pfapirbs;
    }

    return err;
}

ERR CRevertSnapshot::ErrRBSCreateOrLoadRbsGen(
    long lRBSGen, 
    LOGTIME tmPrevGen,
    _Out_bytecap_c_(cbOSFSAPI_MAX_PATHW) PWSTR wszRBSAbsFilePath,
    _Out_bytecap_c_(cbOSFSAPI_MAX_PATHW) PWSTR wszRBSAbsLogDirPath )
{
    WCHAR       wszAbsDirRootPath[ IFileSystemAPI::cchPathMax ];
    WCHAR       wszRBSAbsDirPath[ IFileSystemAPI::cchPathMax ];
    WCHAR       wszRBSAbsLogPath[ IFileSystemAPI::cchPathMax ];
    
    PCWSTR      wszBaseName             = SzParam( m_pinst, JET_paramBaseName );
    ERR         err                     = JET_errSuccess;
    BOOL        fRBSFileExists          = fFalse;
    BOOL        fRBSGenDirExists        = fFalse;
    BOOL        fLogDirCreated          = fFalse;
    BOOL        fRBSFileCreated         = fFalse;
    BOOL        fRBSGenDirCreated       = fFalse;
    QWORD       cbDefaultFileSize       = QWORD( 2 * sizeof( RBSFILEHDR ) );
    
    if ( NULL == wszBaseName || 0 == *wszBaseName )
    {
        return ErrERRCheck(JET_errInvalidParameter);
    }

    Call( ErrRBSAbsRootDir( m_pinst, wszAbsDirRootPath, sizeof( wszAbsDirRootPath ) ) );
    Call( ErrRBSFilePathForGen_( wszAbsDirRootPath, wszBaseName, m_pinst->m_pfsapi, wszRBSAbsDirPath, sizeof( wszRBSAbsDirPath ), wszRBSAbsFilePath, cbOSFSAPI_MAX_PATHW, lRBSGen ) );

    Assert ( sizeof( wszRBSAbsLogPath ) > ( LOSStrLengthW( wszRBSAbsDirPath ) + LOSStrLengthW( wszRBSLogDir ) + 1 + 1 ) * sizeof(WCHAR) );
    Call( ErrOSStrCbCopyW( wszRBSAbsLogPath, sizeof( wszRBSAbsLogPath ), wszRBSAbsDirPath ) );
    Call( ErrOSStrCbAppendW( wszRBSAbsLogPath, sizeof( wszRBSAbsLogPath ), wszRBSLogDir ) );
    Call( m_pinst->m_pfsapi->ErrPathFolderNorm( wszRBSAbsLogPath, sizeof( wszRBSAbsLogPath ) ) );

    fRBSGenDirExists = ( ErrUtilPathExists( m_pinst->m_pfsapi, wszRBSAbsDirPath ) == JET_errSuccess );

    if ( !fRBSGenDirExists )
    {
        Call( ErrUtilCreatePathIfNotExist( m_pinst->m_pfsapi, wszRBSAbsFilePath, NULL, 0 ) );
        fRBSGenDirCreated = fTrue;
    }

    fRBSFileExists = ( ErrUtilPathExists( m_pinst->m_pfsapi, wszRBSAbsFilePath ) == JET_errSuccess );

    if ( !fRBSFileExists )
    {
        TraceContextScope tcHeader( iorpRBS, iorsHeader );

        Call( CIOFilePerf::ErrFileCreate(
                        m_pinst->m_pfsapi,
                        m_pinst,
                        wszRBSAbsFilePath,
                        BoolParam( m_pinst, JET_paramUseFlushForWriteDurability ) ? IFileAPI::fmfStorageWriteBack : IFileAPI::fmfRegular,
                        iofileRBS,
                        QwInstFileID( qwRBSFileID, m_pinst->m_iInstance, lRBSGen ),
                        &m_pfapiRBS ) );

        fRBSFileCreated = true;

        Call( m_pfapiRBS->ErrSetSize( *tcHeader, cbDefaultFileSize, fTrue, qosIONormal ) );

        RBSInitFileHdr( lRBSGen, m_prbsfilehdrCurrent, tmPrevGen );

        Call( ErrUtilWriteRBSHeaders( m_pinst, m_pinst->m_pfsapi, NULL, m_prbsfilehdrCurrent, m_pfapiRBS ) );
    }
    else
    {
        Call( ErrRBSLoadRbsGen( m_pinst, wszRBSAbsFilePath, lRBSGen, m_prbsfilehdrCurrent, &m_pfapiRBS ) );
    }

    m_cNextFlushSegment = m_cNextWriteSegment = m_cNextActiveSegment = IsegRBSSegmentOfFileOffset( m_prbsfilehdrCurrent->rbsfilehdr.le_cbLogicalFileSize );
    if ( m_pActiveBuffer != NULL )
    {
        m_pActiveBuffer->Reset( m_cNextActiveSegment );
    }

    Call( ErrOSStrCbCopyW( wszRBSAbsLogDirPath, cbOSFSAPI_MAX_PATHW, wszRBSAbsLogPath ) );

    if ( ErrUtilPathExists( m_pinst->m_pfsapi, wszRBSAbsLogPath ) != JET_errSuccess )
    {
        Call( ErrUtilCreatePathIfNotExist( m_pinst->m_pfsapi, wszRBSAbsLogPath, NULL, 0 ) );
        fLogDirCreated = fTrue;
    }

    RBSLogCreateOrLoadEvent( m_pinst, wszRBSAbsFilePath, fRBSFileCreated, fTrue, JET_errSuccess );
    return JET_errSuccess;

HandleError:

    RBSLogCreateOrLoadEvent( m_pinst, wszRBSAbsFilePath, fRBSFileCreated, fFalse, err );
    if ( fRBSFileCreated )
    {
        CallSx( m_pinst->m_pfsapi->ErrFileDelete( wszRBSAbsFilePath ), JET_errFileNotFound );
    }

    if ( fLogDirCreated )
    {
        CallSx( m_pinst->m_pfsapi->ErrFolderRemove( wszRBSAbsLogPath ), JET_errFileNotFound );
    }

    if ( fRBSGenDirCreated )
    {
        CallSx( m_pinst->m_pfsapi->ErrFolderRemove( wszRBSAbsDirPath ), JET_errFileNotFound );
    }

    return err;
}

LOCAL ERR ErrRBSFindAttachInfoForDBName( _In_ RBSFILEHDR* prbsfilehdrCurrent, _In_ PCWSTR wszDatabaseName, _Out_ RBSATTACHINFO** prbsattachinfo )
{
    Assert( prbsattachinfo );

    ERR err;
    BOOL fFound = fFalse;

    *prbsattachinfo = (RBSATTACHINFO*) prbsfilehdrCurrent->rgbAttach;

    for ( const BYTE * pbT = prbsfilehdrCurrent->rgbAttach; 0 != *pbT; pbT += sizeof( RBSATTACHINFO ), *prbsattachinfo = (RBSATTACHINFO*) pbT )
    {
        if ( (*prbsattachinfo)->FPresent() == 0 )
        {
            break;
        }
        
        CAutoWSZ wszDatabase;
        CallR( wszDatabase.ErrSet( (*prbsattachinfo)->wszDatabaseName ) );
        if ( UtilCmpFileName( wszDatabaseName, wszDatabase ) == 0 )
        {
            fFound = fTrue;
            break;
        }
    }

    return fFound ? JET_errSuccess : ErrERRCheck( errRBSAttachInfoNotFound );
}

LOCAL ERR ErrRBSRollAttachInfo( 
    BOOL fPrevRBSValid,
    RBSFILEHDR* prbsfilehdrPrev,
    RBSATTACHINFO* prbsattachinfoNew,
    _In_ PCWSTR wszDatabaseName,
    LONG lGenMinRequired,
    LONG lGenMaxRequired,
    SIGNATURE signDb, 
    SIGNATURE signDbHdrFlush )
{
    RBSATTACHINFO* prbsattachinfoOld           = NULL;
    JET_ERR err                             = fPrevRBSValid ? ErrRBSFindAttachInfoForDBName( prbsfilehdrPrev, wszDatabaseName, &prbsattachinfoOld ) : JET_errRBSHeaderCorrupt;
    LittleEndian<DBTIME>  dbtimePrevDirtied = ( err == JET_errSuccess ) ? prbsattachinfoOld->DbtimeDirtied() : 0;

    return ErrRBSInitAttachInfo( prbsattachinfoNew, wszDatabaseName, lGenMinRequired, lGenMaxRequired, dbtimePrevDirtied, signDb, signDbHdrFlush );
}


LOCAL ERR ErrRBSRollAttachInfos( INST* pinst, RBSFILEHDR* prbsfilehdrCurrent, RBSFILEHDR* prbsfilehdrPrev, BOOL fPrevRBSValid, BOOL fInferFromRstmap )
{
    Assert( pinst );
    Assert( prbsfilehdrCurrent );
    Assert( prbsfilehdrPrev );

    RBSATTACHINFO* prbsattachinfoCur    = (RBSATTACHINFO*) prbsfilehdrCurrent->rgbAttach;
    ERR err                             = JET_errSuccess;

    if ( fInferFromRstmap )
    {
        Assert( pinst->m_plog );

        RSTMAP          *rgrstmap   = pinst->m_plog->Rgrstmap();
        INT             irstmapMac  = pinst->m_plog->IrstmapMac();

        for ( INT irstmap = 0; irstmap < irstmapMac; irstmap++ )
        {
            const RSTMAP * const prstmap = rgrstmap + irstmap;

            if ( prstmap->fFileNotFound )
            {
                continue;
            }

            Call( ErrRBSRollAttachInfo( fPrevRBSValid, prbsfilehdrPrev, prbsattachinfoCur, prstmap->wszNewDatabaseName, prstmap->lGenMinRequired, prstmap->lGenMaxRequired, prstmap->signDatabase, prstmap->signDatabaseHdrFlush ) );
            prbsattachinfoCur++;
        }
    }
    else
    {
        for ( DBID dbid = dbidUserLeast; dbid < dbidMax; dbid++ )
        {
            if ( pinst->m_mpdbidifmp[ dbid ] >= g_ifmpMax )
            {
                continue;
            }

            FMP* pfmp                       = &g_rgfmp[ pinst->m_mpdbidifmp[ dbid ] ];
            PdbfilehdrReadOnly pdbHeader    = pfmp->Pdbfilehdr();

           Call( ErrRBSRollAttachInfo( fPrevRBSValid, prbsfilehdrPrev, prbsattachinfoCur, pfmp->WszDatabaseName(), pdbHeader->le_lGenMinRequired, pdbHeader->le_lGenMaxRequired, pdbHeader->signDb, pdbHeader->signDbHdrFlush ) );
            prbsattachinfoCur++;
        }
    }

HandleError:
    return err;
}

VOID *CSnapshotBuffer::s_pReserveBuffer = NULL;


CRevertSnapshot::CRevertSnapshot( __in INST* const pinst )
    : CZeroInit( sizeof( CRevertSnapshot ) ),
    m_pinst ( pinst ),
    m_cresRBSBuf( pinst ),
    m_critBufferLock( CLockBasicInfo( CSyncBasicInfo( szRBSBuf ), rankRBSBuf, 0 ) ),
    m_critWriteLock( CLockBasicInfo( CSyncBasicInfo( szRBSWrite ), rankRBSWrite, 0 ) )
{
    Assert( pinst );
}

CRevertSnapshot::~CRevertSnapshot( )
{
    FreeFileApi( );
    FreeHdr( );
    FreePaths( );

    delete m_pActiveBuffer;
    m_pActiveBuffer = NULL;
    for ( CSnapshotBuffer *pBuffer = m_pBuffersToWrite; pBuffer != NULL; )
    {
        CSnapshotBuffer *pBufferNext = pBuffer->m_pNextBuffer;
        delete pBuffer;
        pBuffer = pBufferNext;
    }
    m_pBuffersToWrite = NULL;
    m_pBuffersToWriteLast = NULL;

    m_cresRBSBuf.Term();

    delete m_pReadBuffer;
    m_pReadBuffer = NULL;

    CSnapshotBuffer::FreeReserveBuffer();
}

ERR CRevertSnapshot::ErrResetHdr( )
{
    ERR err = JET_errSuccess;
    FreeHdr( );
    Alloc( m_prbsfilehdrCurrent = (RBSFILEHDR *)PvOSMemoryPageAlloc( sizeof(RBSFILEHDR), NULL ) );

HandleError:
    return err;
}

VOID CRevertSnapshot::EnterDbHeaderFlush( CRevertSnapshot* prbs, __out SIGNATURE* const psignRBSHdrFlush )
{
    if ( prbs != NULL && prbs->m_fInitialized && !prbs->m_fInvalid )
    {
        UtilMemCpy( psignRBSHdrFlush, &prbs->m_prbsfilehdrCurrent->rbsfilehdr.signRBSHdrFlush, sizeof( SIGNATURE ) );
    }
    else
    {
        SIGResetSignature( psignRBSHdrFlush );
    }
}

DBTIME
CRevertSnapshot::GetDbtimeForFmp( FMP *pfmp )
{
    Assert( m_fInitialized );
    Assert( !m_fInvalid );

    RBSATTACHINFO *pAttachInfo = NULL;
    ERR err = ErrRBSFindAttachInfoForDBName( m_prbsfilehdrCurrent, pfmp->WszDatabaseName(), &pAttachInfo );
    CallS( err );
    Assert( pAttachInfo != NULL );
    return pAttachInfo->DbtimeDirtied();
}

ERR CRevertSnapshot::ErrSetDbtimeForFmp( FMP *pfmp, DBTIME dbtime )
{
    Assert( m_fInitialized );
    Assert( !m_fInvalid );

    ENTERCRITICALSECTION critWrite( &m_critWriteLock );
    RBSATTACHINFO *pAttachInfo = NULL;
    ERR err = ErrRBSFindAttachInfoForDBName( m_prbsfilehdrCurrent, pfmp->WszDatabaseName(), &pAttachInfo );
    CallS( err );
    Assert( pAttachInfo != NULL );
    Assert( pAttachInfo->DbtimeDirtied() == 0 );
    pAttachInfo->SetDbtimeDirtied( dbtime );
    return ErrUtilWriteRBSHeaders( m_pinst, m_pinst->m_pfsapi, NULL, m_prbsfilehdrCurrent, m_pfapiRBS );
}

ERR CRevertSnapshot::ErrSetRBSFileApi( __in IFileAPI *pfapiRBS )
{
    Assert( !m_fInitialized );
    Assert( !m_fInvalid );
    Assert( m_pfapiRBS == NULL );
    Assert( m_wszRBSCurrentFile == NULL );
    Assert( m_prbsfilehdrCurrent == NULL );
    Assert( pfapiRBS != NULL );

    ERR err = JET_errSuccess;
    m_pfapiRBS = pfapiRBS;

    WCHAR       wszRBSAbsFilePath[ IFileSystemAPI::cchPathMax ];

    Call( pfapiRBS->ErrPath( wszRBSAbsFilePath ) );
    
    const SIZE_T    cchRBSName       = sizeof( WCHAR ) * ( LOSStrLengthW( wszRBSAbsFilePath ) + 1 );
    Alloc( m_wszRBSCurrentFile = static_cast<WCHAR *>( PvOSMemoryHeapAlloc( cchRBSName ) ) );
    Call( ErrOSStrCbCopyW( m_wszRBSCurrentFile, cchRBSName, wszRBSAbsFilePath ) );

    Alloc( m_prbsfilehdrCurrent = (RBSFILEHDR *)PvOSMemoryPageAlloc( sizeof(RBSFILEHDR), NULL ) );    

    Call( ErrUtilReadShadowedHeader( m_pinst, m_pinst->m_pfsapi, m_pfapiRBS, (BYTE*) m_prbsfilehdrCurrent, sizeof( RBSFILEHDR ), -1, urhfNoAutoDetectPageSize | urhfReadOnly | urhfNoEventLogging ) );
    m_fInitialized = fTrue;

HandleError:
    return err;
}

ERR CRevertSnapshot::ErrRBSInit( BOOL fRBSCreateIfRequired )
{    
    
    if ( !BoolParam( m_pinst, JET_paramEnableRBS ) || m_pinst->m_plog->FLogDisabled() )
    {
        return JET_errSuccess;
    }

    WCHAR   wszAbsDirRootPath[ IFileSystemAPI::cchPathMax ];
    WCHAR   wszRBSAbsFilePath[ IFileSystemAPI::cchPathMax ];
    WCHAR   wszRBSAbsLogDirPath[ IFileSystemAPI::cchPathMax ];

    LONG    rbsGenMax;
    LONG    rbsGenMin;
    ERR     err             = JET_errSuccess;
    LOGTIME defaultLogTime  = { 0 };

    Call( ErrRBSInitPaths_( m_pinst, &m_wszRBSAbsRootDirPath, &m_wszRBSBaseName ) );

    Call( ErrUtilCreatePathIfNotExist( m_pinst->m_pfsapi, m_wszRBSAbsRootDirPath, wszAbsDirRootPath, sizeof ( wszAbsDirRootPath ) ) );
    Call( m_pinst->m_pfsapi->ErrPathFolderNorm( wszAbsDirRootPath, sizeof( wszAbsDirRootPath ) ) );

    Assert( LOSStrLengthW( wszAbsDirRootPath ) > 0 );
    Assert( LOSStrLengthW( m_wszRBSBaseName ) > 0 );

    Call( ErrRBSGetLowestAndHighestGen_( m_pinst->m_pfsapi, wszAbsDirRootPath, m_wszRBSBaseName, &rbsGenMin, &rbsGenMax ) );

    Call( ErrResetHdr() );

    if ( rbsGenMax == 0 )
    {
        if ( !fRBSCreateIfRequired )
        {
            RBSLogCreateSkippedEvent( m_pinst, SzParam( m_pinst, JET_paramRBSFilePath ), errRBSRequiredRangeTooLarge, JET_errFileNotFound );
            Error( ErrERRCheck( errRBSRequiredRangeTooLarge ) );
        }

        rbsGenMax = 1;
    }

    err = ErrRBSCreateOrLoadRbsGen( rbsGenMax, defaultLogTime, wszRBSAbsFilePath, wszRBSAbsLogDirPath );
    
    if ( err == JET_errReadVerifyFailure || err == JET_errFileInvalidType || err == JET_errBadRBSVersion || err == JET_errRBSInvalidSign )
    {
        Call( ErrResetHdr() );
        FreeFileApi();

        if ( m_pinst->m_prbscleaner != NULL )
        {
            m_pinst->m_prbscleaner->SetFirstValidGen( rbsGenMax + 1 );
        }

        if ( fRBSCreateIfRequired )
        {
            Call( ErrRBSCreateOrLoadRbsGen( rbsGenMax + 1, defaultLogTime, wszRBSAbsFilePath, wszRBSAbsLogDirPath ) );
        }
        else
        {
            RBSLogCreateSkippedEvent( m_pinst, SzParam( m_pinst, JET_paramRBSFilePath ), errRBSRequiredRangeTooLarge, err );
            Error( ErrERRCheck( errRBSRequiredRangeTooLarge ) );
        }
    }
    Call( err );

    Assert( m_cNextActiveSegment > 0 );
    if ( m_cNextActiveSegment > cRBSSegmentMax )
    {
        Error( ErrERRCheck( JET_errOutOfRBSSpace ) );
    }

    const SIZE_T    cchRBSName       = sizeof( WCHAR ) * ( LOSStrLengthW( wszRBSAbsFilePath ) + 1 );
    m_wszRBSCurrentFile = static_cast<WCHAR *>( PvOSMemoryHeapAlloc( cchRBSName ) );
    Alloc( m_wszRBSCurrentFile );

    const SIZE_T    cchRBSLogDirName       = sizeof( WCHAR ) * ( LOSStrLengthW( wszRBSAbsLogDirPath ) + 1 );
    m_wszRBSCurrentLogDir = static_cast<WCHAR *>( PvOSMemoryHeapAlloc( cchRBSLogDirName ) );
    Alloc( m_wszRBSCurrentLogDir );

    Call( ErrOSStrCbCopyW( m_wszRBSCurrentFile, cchRBSName, wszRBSAbsFilePath ) );
    Call( ErrOSStrCbCopyW( m_wszRBSCurrentLogDir, cchRBSLogDirName, wszRBSAbsLogDirPath ) );

    Call( m_cresRBSBuf.ErrInit( JET_residRBSBuf ) );

    Alloc( m_pActiveBuffer = new (&m_cresRBSBuf) CSnapshotBuffer( m_cNextActiveSegment, &m_cresRBSBuf ) );
    Call( m_pActiveBuffer->ErrAllocBuffer() );

    CSnapshotBuffer::PreAllocReserveBuffer();

    m_fInitialized = fTrue;

    Assert( m_prbsfilehdrCurrent );
    Assert( m_prbsfilehdrCurrent->rbsfilehdr.le_lGeneration > 0 );

    if ( m_pinst->m_prbscleaner && !m_prbsfilehdrCurrent->rbsfilehdr.tmPrevGen.FIsSet() )
    {
        m_pinst->m_prbscleaner->SetFirstValidGen( m_prbsfilehdrCurrent->rbsfilehdr.le_lGeneration );
    }

    return JET_errSuccess;

HandleError:
    FreeFileApi();
    FreeHdr();
    delete m_pActiveBuffer;
    m_pActiveBuffer = NULL;

    return err;
}

ERR CRevertSnapshot::ErrRBSInvalidate()
{
    Assert( FInitialized() );

    for ( DBID dbid = dbidUserLeast; dbid < dbidMax; ++dbid )
    {
        IFMP        ifmp    = m_pinst->m_mpdbidifmp[ dbid ];
        if ( ifmp >= g_ifmpMax )
            continue;

        FMP         *pfmp   = &g_rgfmp[ifmp];
        pfmp->ResetRBSOn();
        pfmp->ResetNeedUpdateDbtimeBeginRBS();
    }

    m_fInvalid = fTrue;

    SIGResetSignature( &m_prbsfilehdrCurrent->rbsfilehdr.signRBSHdrFlush );
    return ErrUtilWriteRBSHeaders( m_pinst, m_pinst->m_pfsapi, NULL, m_prbsfilehdrCurrent, m_pfapiRBS );
}

VOID CRevertSnapshot::RBSLogSpaceUsage()
{
    Assert( m_pinst );
    Assert( m_pinst->m_plog );
    Assert( FInitialized() );

    WCHAR   wszTimeCreate[32], wszDateCreate[32], wszTimePrevRun[32], wszDatePrevRun[32], wszSizeGrown[32], wszNumLogs[16];

    {
    ENTERCRITICALSECTION critWrite( &m_critWriteLock );
    _int64 ftCurrent    = UtilGetCurrentFileTime();
    LONG   lGenCurrent  = m_pinst->m_plog->LGGetCurrentFileGenNoLock();

    if ( m_ftSpaceUsageLastLogged == 0 )
    {
        m_ftSpaceUsageLastLogged        = ftCurrent;
        m_lGenSpaceUsageLastRun         = lGenCurrent;
        m_cbFileSizeSpaceUsageLastRun   = m_prbsfilehdrCurrent->rbsfilehdr.le_cbLogicalFileSize;
        return;
    }

    if ( UtilConvertFileTimeToSeconds( ftCurrent - m_ftSpaceUsageLastLogged ) > csecSpaceUsagePeriodicLog )
    {
        QWORD cbSpaceGrowth = m_prbsfilehdrCurrent->rbsfilehdr.le_cbLogicalFileSize - m_cbFileSizeSpaceUsageLastRun;
        LONG  cLogsGrowth   = lGenCurrent - m_lGenSpaceUsageLastRun;
        __int64 ftCreate    = ConvertLogTimeToFileTime( &m_prbsfilehdrCurrent->rbsfilehdr.tmCreate );

        size_t  cchRequired;

        ErrUtilFormatFileTimeAsTimeWithSeconds( ftCreate, wszTimeCreate, _countof( wszTimeCreate ), &cchRequired );
        ErrUtilFormatFileTimeAsDate( ftCreate, wszDateCreate, _countof( wszDateCreate ), &cchRequired );

        ErrUtilFormatFileTimeAsTimeWithSeconds( m_ftSpaceUsageLastLogged, wszTimePrevRun, _countof( wszTimePrevRun ), &cchRequired );
        ErrUtilFormatFileTimeAsDate( m_ftSpaceUsageLastLogged, wszDatePrevRun, _countof( wszDatePrevRun ), &cchRequired );

        OSStrCbFormatW( wszSizeGrown, sizeof( wszSizeGrown ), L"%I64u", cbSpaceGrowth ),
        OSStrCbFormatW( wszNumLogs, sizeof( wszNumLogs ), L"%d", cLogsGrowth );

        m_cbFileSizeSpaceUsageLastRun   = m_prbsfilehdrCurrent->rbsfilehdr.le_cbLogicalFileSize;
        m_lGenSpaceUsageLastRun         = lGenCurrent;
        m_ftSpaceUsageLastLogged        = ftCurrent;
    }
    else
    {
        return;
    }
    }

    const WCHAR* rgcwsz[] =
    {
        m_wszRBSCurrentFile,
        OSFormatW( L"%ws %ws", wszTimeCreate, wszDateCreate ),
        wszSizeGrown,
        OSFormatW( L"%ws %ws", wszTimePrevRun, wszDatePrevRun ),
        wszNumLogs
    };

    UtilReportEvent(
        eventInformation,
        GENERAL_CATEGORY,
        RBS_SPACE_GROWTH_ID,
        5,
        rgcwsz,
        0,
        NULL,
        m_pinst );
}

ERR CRevertSnapshot::ErrRBSRecordDbAttach( __in FMP* const pfmp )
{
    ERR err                     = JET_errSuccess;


    if ( pfmp->ErrDBFormatFeatureEnabled( JET_efvRevertSnapshot ) < JET_errSuccess )
    {
        Assert( !m_fInitialized );
        return JET_errSuccess;
    }

    if ( m_fInvalid )
    {
        return JET_errSuccess;
    }

    RBSATTACHINFO* prbsattachinfo   = NULL;

    err = ErrRBSFindAttachInfoForDBName( m_prbsfilehdrCurrent, pfmp->WszDatabaseName(), &prbsattachinfo );

    Assert( prbsattachinfo );

    {
    ENTERCRITICALSECTION critWrite( &m_critWriteLock );

    {
    PdbfilehdrReadOnly pdbfilehdr    = pfmp->Pdbfilehdr();

    if ( err == JET_errSuccess )
    {
        Assert( memcmp( &pdbfilehdr->signDb, &prbsattachinfo->signDb, sizeof( SIGNATURE ) ) == 0 );
        Assert( FRBSCheckForDbConsistency( &pdbfilehdr->signDbHdrFlush, &pdbfilehdr->signRBSHdrFlush, &prbsattachinfo->signDbHdrFlush,  &m_prbsfilehdrCurrent->rbsfilehdr.signRBSHdrFlush ) );
    }
    else
    {
        if( pdbfilehdr->Dbstate() != JET_dbstateJustCreated && pdbfilehdr->Dbstate() != JET_dbstateCleanShutdown )
        {
            Call( ErrRBSInvalidate() );
            goto HandleError;
        }

        if ( (BYTE *)(prbsattachinfo + 1) > (m_prbsfilehdrCurrent->rgbAttach + sizeof( m_prbsfilehdrCurrent->rgbAttach )) )
        {
            Error( ErrERRCheck( JET_errBufferTooSmall ) );
        }
        Assert( prbsattachinfo->FPresent() == 0 );


        Call( ErrRBSInitAttachInfo( prbsattachinfo, pfmp->WszDatabaseName(), pdbfilehdr->le_lGenMinRequired, pdbfilehdr->le_lGenMaxRequired, 0, pdbfilehdr->signDb, pdbfilehdr->signDbHdrFlush ) );
        Call( ErrUtilWriteRBSHeaders( m_pinst, m_pinst->m_pfsapi, NULL, m_prbsfilehdrCurrent, m_pfapiRBS ) );
    }
    }
    }

    pfmp->SetRBSOn();

    Call( ErrCaptureDbAttach( pfmp ) );
    Call( ErrCaptureDbHeader( pfmp ) );
    Call( ErrFlushAll() );

HandleError:
    return err;
}

ERR CRevertSnapshot::ErrRBSInitDBFromRstmap( __in const RSTMAP* const prstmap, LONG lgenLow, LONG lgenHigh )
{
    Assert( prstmap );
    Assert( m_fInitialized );
    Assert( !m_fInvalid );
    
    if ( m_prbsfilehdrCurrent->rbsfilehdr.bLogsCopied || m_prbsfilehdrCurrent->rbsfilehdr.le_lGenMinLogCopied > 0 )
    {
        lgenLow = m_prbsfilehdrCurrent->rbsfilehdr.le_lGenMinLogCopied;
        lgenHigh = m_prbsfilehdrCurrent->rbsfilehdr.le_lGenMaxLogCopied;
    }

    ERR err                     = JET_errSuccess;


    RBSATTACHINFO* prbsattachinfo   = NULL;

    err = ErrRBSFindAttachInfoForDBName( m_prbsfilehdrCurrent, prstmap->wszNewDatabaseName, &prbsattachinfo );

    if ( err == JET_errSuccess )
    {
        Assert( prbsattachinfo->LGenMaxRequired() >= prbsattachinfo->LGenMinRequired() );
        Assert( prbsattachinfo->LGenMaxRequired() <= lgenHigh || prbsattachinfo->LGenMaxRequired() == 0 );
        Assert( prbsattachinfo->LGenMinRequired() >= lgenLow  || prbsattachinfo->LGenMaxRequired() == 0 );

        if ( memcmp( &prstmap->signDatabase, &prbsattachinfo->signDb, sizeof( SIGNATURE ) ) != 0 ||
            !FRBSCheckForDbConsistency( &prstmap->signDatabaseHdrFlush, &prstmap->signRBSHdrFlush, &prbsattachinfo->signDbHdrFlush,  &m_prbsfilehdrCurrent->rbsfilehdr.signRBSHdrFlush ) )
        {
             Error( ErrERRCheck( JET_errRBSDbMismatch ) );
        }

    }
    else
    {
        Assert( prstmap->lGenMaxRequired >= prstmap->lGenMinRequired );
        Assert( prstmap->lGenMinRequired >= lgenLow );
        Assert( prstmap->lGenMaxRequired <= lgenHigh );

        if ( (BYTE *)(prbsattachinfo + 1) > (m_prbsfilehdrCurrent->rgbAttach + sizeof( m_prbsfilehdrCurrent->rgbAttach )) )
        {
            Error( ErrERRCheck( JET_errBufferTooSmall ) );
        }
        Assert( prbsattachinfo->FPresent() == 0 );


        Call( ErrRBSInitAttachInfo( prbsattachinfo, prstmap->wszNewDatabaseName, prstmap->lGenMinRequired, prstmap->lGenMaxRequired, 0, prstmap->signDatabase, prstmap->signDatabaseHdrFlush ) );
        Call( ErrUtilWriteRBSHeaders( m_pinst, m_pinst->m_pfsapi, NULL, m_prbsfilehdrCurrent, m_pfapiRBS ) );
    }

HandleError:
    return err;
}

ERR CRevertSnapshot::ErrRBSInitFromRstmap( INST* pinst )
{
    Assert( pinst );
    Assert( pinst->m_plog );
    Assert( !pinst->m_prbs );

    ERR err = JET_errSuccess;
    LONG lgenLow = 0;
    LONG lgenHigh = 0;
    BOOL fRBSCreateIfRequired = fFalse;
    CRevertSnapshot* prbs = NULL;

    if ( pinst->m_plog->FLogDisabled() ||
        !BoolParam( pinst, JET_paramEnableRBS ) || 
        !pinst->m_plog->FRBSFeatureEnabledFromRstmap() )
    {
        return JET_errSuccess;
    }

    pinst->m_plog->LoadRBSGenerationFromRstmap( &lgenLow, &lgenHigh );

    Assert( lgenLow >= 0 );
    Assert( lgenHigh >= 0 );
    Assert( lgenHigh - lgenLow >= 0 );
    
    fRBSCreateIfRequired = (lgenHigh - lgenLow) <= ( (LONG) UlParam( pinst, JET_paramFlight_RBSMaxRequiredRange ) );

    Alloc( prbs = new CRevertSnapshot( pinst ) );
    Call( prbs->ErrRBSInit( fRBSCreateIfRequired ) );

    RSTMAP          *rgrstmap   = pinst->m_plog->Rgrstmap();
    INT             irstmapMac  = pinst->m_plog->IrstmapMac();

    for ( INT irstmap = 0; irstmap < irstmapMac; irstmap++ )
    {
        const RSTMAP * const prstmap = rgrstmap + irstmap;

        if ( prstmap->fFileNotFound )
        {
            continue;
        }

        err = prbs->ErrRBSInitDBFromRstmap( prstmap, lgenLow, lgenHigh );

        if ( err == JET_errRBSDbMismatch )
        {
            if ( pinst->m_prbscleaner && prbs->FInitialized() )
            {
                LONG lRBSCurrentGen = prbs->RBSFileHdr()->rbsfilehdr.le_lGeneration;
                pinst->m_prbscleaner->SetFirstValidGen( lRBSCurrentGen + 1 );
            }

            if ( fRBSCreateIfRequired )
            {
                Call( prbs->ErrRollSnapshot( fFalse, fTrue ) );
            }
            else
            {
                RBSLogCreateSkippedEvent( pinst, SzParam( pinst, JET_paramRBSFilePath ), errRBSRequiredRangeTooLarge, err );
                Error( ErrERRCheck( errRBSRequiredRangeTooLarge ) );
            }
        }
        else
        {
            Call( err );
        }
    }

    Call( prbs->ErrRBSCopyRequiredLogs( fTrue ) );
    pinst->m_prbs = prbs;
    return JET_errSuccess;

HandleError:
    if ( prbs != NULL )
    {
        delete prbs;
        prbs = NULL;
    }

    if ( err == errRBSRequiredRangeTooLarge )
    {
        err = JET_errSuccess;
    }

    return err;
}

VOID RBSICompressPreImage(
    INST *pinst,
    IFMP ifmp,
    PGNO pgno,
    const LONG cbPage,
    DATA &dataToSet,
    BYTE *pbDataDehydrated,
    BYTE *pbDataCompressed,
    ULONG *pcompressionPerformed )
{
    *pcompressionPerformed = 0;
    INT cbDataCompressedActual = 0;

#if 0
    {
    CPAGE cpageT;
    if ( dataToSet.Cb() == cbPage )
    {
        cpageT.LoadPage( ifmp, pgno, dataToSet.Pv(), dataToSet.Cb() );
    }
    else
    {
        cpageT.LoadDehydratedPage( ifmp, pgno, dataToSet.Pv(), dataToSet.Cb(), cbPage );
    }
    cpageT.LoggedDataChecksum();
    cpageT.UnloadPage();
    }
#endif

    if ( dataToSet.Cb() == cbPage )
    {
        memcpy( pbDataDehydrated, dataToSet.Pv(), dataToSet.Cb() );
        CPAGE cpageT;
        cpageT.LoadPage( ifmp, pgno, pbDataDehydrated, dataToSet.Cb() );
        if ( cpageT.FPageIsDehydratable( (ULONG *)&cbDataCompressedActual, fTrue ) )
        {
            cpageT.DehydratePage( cbDataCompressedActual, fTrue );
            dataToSet.SetPv( pbDataDehydrated );
            dataToSet.SetCb( cbDataCompressedActual );
            *pcompressionPerformed |= fRBSPreimageDehydrated;
        }
        cpageT.UnloadPage();
    }
    else
    {
        Assert( dataToSet.Cb() < cbPage );
        *pcompressionPerformed |= fRBSPreimageDehydrated;
    }

    CompressFlags compressFlags = compressXpress;
    if ( BoolParam( pinst, JET_paramFlight_EnableXpress10Compression ) )
    {
        compressFlags = CompressFlags( compressFlags | compressXpress10 );
    }

    if ( ErrPKCompressData( dataToSet, compressFlags, pinst, pbDataCompressed, CbPKCompressionBuffer(), &cbDataCompressedActual ) >= JET_errSuccess &&
         cbDataCompressedActual < dataToSet.Cb() )
    {
        dataToSet.SetPv( pbDataCompressed );
        dataToSet.SetCb( cbDataCompressedActual );
        *pcompressionPerformed |= fRBSPreimageXpress;
    }
}

ERR ErrRBSDecompressPreimage(
    DATA &data,
    const LONG cbPage,
    BYTE *pbDataDecompressed,
    PGNO pgno,
    ULONG fFlags )
{
    ERR err;

    if ( fFlags & fRBSPreimageXpress )
    {
        INT cbActual = 0;
        CallR( ErrPKDecompressData( data, ifmpNil, pgno, pbDataDecompressed, cbPage, &cbActual ) );
        Assert( err != JET_wrnBufferTruncated );
        data.SetPv( pbDataDecompressed );
        data.SetCb( cbActual );
    }

    if ( ( fFlags & fRBSPreimageDehydrated ) || data.Cb() < cbPage )
    {
        if ( fFlags & fRBSPreimageXpress )
        {
            Assert( data.Pv() == pbDataDecompressed );
        }
        else
        {
            memcpy( pbDataDecompressed, data.Pv(), data.Cb() );
        }
        CPAGE cpageT;
        cpageT.LoadDehydratedPage( ifmpNil, pgno, pbDataDecompressed, data.Cb(), cbPage );
        cpageT.RehydratePage();
        data.SetPv( pbDataDecompressed );
        Assert( cpageT.CbBuffer() == (ULONG)cbPage );
        data.SetCb( cbPage );
        cpageT.UnloadPage();
    }

    Assert( data.Cb() == cbPage );
    return JET_errSuccess;
}

#ifdef ENABLE_JET_UNIT_TEST

JETUNITTEST( FRBSCheckForDbConsistency, Test )
{
    SIGNATURE signDbHdrFlush, sigRBSHdrFlush;
    SIGGetSignature( &signDbHdrFlush );
    SIGGetSignature( &sigRBSHdrFlush );

    SIGNATURE signDbHdrFlushFromDb, sigRBSHdrFlushFromDb, signDbHdrFlushFromRBS, signRBSHdrFlushFromRBS;
    CHECK( FRBSCheckForDbConsistency( &signDbHdrFlushFromDb, &sigRBSHdrFlushFromDb, &signDbHdrFlushFromRBS, &signRBSHdrFlushFromRBS ) == fFalse );

    UtilMemCpy( &signDbHdrFlushFromDb, &signDbHdrFlush, sizeof( SIGNATURE ) );
    UtilMemCpy( &signRBSHdrFlushFromRBS, &sigRBSHdrFlush, sizeof( SIGNATURE ) );
    CHECK( FRBSCheckForDbConsistency( &signDbHdrFlushFromDb, &sigRBSHdrFlushFromDb, &signDbHdrFlushFromRBS, &signRBSHdrFlushFromRBS ) == fFalse );

    UtilMemCpy( &sigRBSHdrFlushFromDb, &sigRBSHdrFlush, sizeof( SIGNATURE ) );
    CHECK( FRBSCheckForDbConsistency( &signDbHdrFlushFromDb, &sigRBSHdrFlushFromDb, &signDbHdrFlushFromRBS, &signRBSHdrFlushFromRBS ) == fTrue );

    SIGResetSignature( &sigRBSHdrFlushFromDb );
    UtilMemCpy( &signDbHdrFlushFromRBS, &signDbHdrFlush, sizeof( SIGNATURE ) );
    CHECK( FRBSCheckForDbConsistency( &signDbHdrFlushFromDb, &sigRBSHdrFlushFromDb, &signDbHdrFlushFromRBS, &signRBSHdrFlushFromRBS ) == fTrue );
}

JETUNITTESTDB( RBSPreImageCompression, Dehydration, dwOpenDatabase )
{
    const ULONG cbPage = 32 * 1024;
    CPAGE cpage, cpageT;
    FMP * pfmp = g_rgfmp + IfmpTest();
    LINE line, lineT;
    INT iline = 0;
    DATA data, dataRec;
    BYTE rgbData[5];
    BYTE *pbDehydrationBuffer = NULL, *pbCompressionBuffer = NULL;
    ULONG compressionPerformed;

    cpage.LoadNewTestPage( cbPage, IfmpTest() );
    memset( rgbData, '0', sizeof(rgbData) );
    dataRec.SetPv( rgbData );
    dataRec.SetCb( sizeof(rgbData) );
    cpage.Insert( 0, &dataRec, 1, 0 );

    data.SetPv( cpage.PvBuffer() );
    data.SetCb( cbPage );
    pbDehydrationBuffer = new BYTE[cbPage];
    pbCompressionBuffer = new BYTE[cbPage];

    PKTermCompression();
    CHECK( JET_errSuccess == ErrPKInitCompression( cbPage, 1024, cbPage ) );

    RBSICompressPreImage( pfmp->Pinst(), pfmp->Ifmp(), cpage.PgnoThis(), cbPage, data, pbDehydrationBuffer, pbCompressionBuffer, &compressionPerformed );
    CHECK( compressionPerformed == fRBSPreimageDehydrated );
    CHECK( data.Pv() == pbDehydrationBuffer );
    CHECK( data.Cb() < cbPage );

    CHECK( JET_errSuccess == ErrRBSDecompressPreimage( data, cbPage, pbCompressionBuffer, cpage.PgnoThis(), compressionPerformed ) );
    CHECK( data.Pv() == pbCompressionBuffer );
    CHECK( data.Cb() == cbPage );

    cpageT.LoadPage( data.Pv(), data.Cb() );
    CHECK( cpage.Clines() == cpageT.Clines() );
    cpage.GetPtrExternalHeader( &line );
    cpageT.GetPtrExternalHeader( &lineT );
    iline = 0;
    while ( fTrue )
    {
        CHECK( line.cb == lineT.cb );
        CHECK( memcmp( line.pv, lineT.pv, line.cb ) == 0 );
        CHECK( line.fFlags == lineT.fFlags );

        if ( iline >= cpage.Clines() )
            break;

        cpage.GetPtr( iline, &line );
        cpageT.GetPtr( iline, &lineT );
        iline++;
    }

    delete[] pbCompressionBuffer;
    delete[] pbDehydrationBuffer;
}


JETUNITTESTDB( RBSPreImageCompression, DehydrationAndXpress, dwOpenDatabase )
{
    const ULONG cbPage = 32 * 1024;
    CPAGE cpage, cpageT;
    FMP * pfmp = g_rgfmp + IfmpTest();
    LINE line, lineT;
    INT iline = 0;
    DATA data, dataRec;
    BYTE rgbData[100];
    BYTE *pbDehydrationBuffer = NULL, *pbCompressionBuffer = NULL;
    ULONG compressionPerformed;

    cpage.LoadNewTestPage( cbPage, IfmpTest() );
    memset( rgbData, '0', sizeof(rgbData) );
    dataRec.SetPv( rgbData );
    dataRec.SetCb( sizeof(rgbData) );
    for ( iline=0; iline<100; iline++ )
    {
        cpage.Insert( iline, &dataRec, 1, 0 );
    }

    data.SetPv( cpage.PvBuffer() );
    data.SetCb( cbPage );
    pbDehydrationBuffer = new BYTE[cbPage];
    pbCompressionBuffer = new BYTE[cbPage];
    
    PKTermCompression();
    CHECK( JET_errSuccess == ErrPKInitCompression( cbPage, 1024, cbPage ) );

    RBSICompressPreImage( pfmp->Pinst(), pfmp->Ifmp(), cpage.PgnoThis(), cbPage, data, pbDehydrationBuffer, pbCompressionBuffer, &compressionPerformed );
    CHECK( compressionPerformed == (fRBSPreimageDehydrated | fRBSPreimageXpress) );
    CHECK( data.Pv() == pbCompressionBuffer );
    CHECK( data.Cb() < cbPage );

    CHECK( JET_errSuccess == ErrRBSDecompressPreimage( data, cbPage, pbDehydrationBuffer, cpage.PgnoThis(), compressionPerformed ) );
    CHECK( data.Pv() == pbDehydrationBuffer );
    CHECK( data.Cb() == cbPage );

    cpageT.LoadPage( data.Pv(), data.Cb() );
    CHECK( cpage.Clines() == cpageT.Clines() );
    cpage.GetPtrExternalHeader( &line );
    cpageT.GetPtrExternalHeader( &lineT );
    iline = 0;
    while ( fTrue )
    {
        CHECK( line.cb == lineT.cb );
        CHECK( memcmp( line.pv, lineT.pv, line.cb ) == 0 );
        CHECK( line.fFlags == lineT.fFlags );

        if ( iline >= cpage.Clines() )
            break;

        cpage.GetPtr( iline, &line );
        cpageT.GetPtr( iline, &lineT );
        iline++;
    }

    delete[] pbCompressionBuffer;
    delete[] pbDehydrationBuffer;
}

JETUNITTESTDB( RBSPreImageCompression, Xpress, dwOpenDatabase )
{
    const ULONG cbPage = 32 * 1024;
    CPAGE cpage;
    FMP * pfmp = g_rgfmp + IfmpTest();
    DATA data, dataRec;
    BYTE rgbData[100];
    BYTE *pbDehydrationBuffer = NULL, *pbCompressionBuffer = NULL;
    ULONG compressionPerformed;

    cpage.LoadNewTestPage( cbPage, IfmpTest() );
    memset( rgbData, '0', sizeof(rgbData) );
    dataRec.SetPv( rgbData );
    dataRec.SetCb( sizeof(rgbData) );
    for ( INT i=0; i<310; i++ )
    {
        cpage.Insert( i, &dataRec, 1, 0 );
    }

    data.SetPv( cpage.PvBuffer() );
    data.SetCb( cbPage );
    pbDehydrationBuffer = new BYTE[cbPage];
    pbCompressionBuffer = new BYTE[cbPage];
    
    PKTermCompression();
    CHECK( JET_errSuccess == ErrPKInitCompression( cbPage, 1024, cbPage ) );

    RBSICompressPreImage( pfmp->Pinst(), pfmp->Ifmp(), cpage.PgnoThis(), cbPage, data, pbDehydrationBuffer, pbCompressionBuffer, &compressionPerformed );
    CHECK( compressionPerformed == fRBSPreimageXpress );
    CHECK( data.Pv() == pbCompressionBuffer );
    CHECK( data.Cb() < cbPage );

    CHECK( JET_errSuccess == ErrRBSDecompressPreimage( data, cbPage, pbDehydrationBuffer, cpage.PgnoThis(), compressionPerformed ) );
    CHECK( data.Pv() == pbDehydrationBuffer );
    CHECK( data.Cb() == cbPage );
    CHECK( memcmp( cpage.PvBuffer(), data.Pv(), cbPage ) == 0 );

    delete[] pbCompressionBuffer;
    delete[] pbDehydrationBuffer;
}

#endif

ERR CRevertSnapshot::ErrCapturePreimage(
        DBID dbid,
        PGNO pgno,
        _In_reads_( cbImage ) const BYTE *pbImage,
        ULONG cbImage,
        RBS_POS *prbsposRecord )
{
    Assert( m_fInitialized );
    Assert( !m_fInvalid );

    ERR err = JET_errSuccess;
    BYTE *pbDataDehydrated = NULL, *pbDataCompressed = NULL;
    RBSDbPageRecord dbRec;
    ULONG fFlags;
    DATA dataRec;
    dataRec.SetPv( (VOID *)pbImage );
    dataRec.SetCb( cbImage );

    Alloc( pbDataDehydrated = PbPKAllocCompressionBuffer() );
    Alloc( pbDataCompressed = PbPKAllocCompressionBuffer() );
    RBSICompressPreImage( m_pinst, m_pinst->m_mpdbidifmp[ dbid ], pgno, g_cbPage, dataRec, pbDataDehydrated, pbDataCompressed, &fFlags );

    dbRec.m_bRecType = rbsrectypeDbPage;
    dbRec.m_usRecLength = sizeof( RBSDbPageRecord ) + dataRec.Cb();
    dbRec.m_dbid = dbid;
    dbRec.m_pgno = pgno;
    dbRec.m_fFlags = fFlags;

    err = ErrCaptureRec( &dbRec, &dataRec, prbsposRecord );

HandleError:
    PKFreeCompressionBuffer( pbDataDehydrated );
    PKFreeCompressionBuffer( pbDataCompressed );

    return err;
}

ERR CRevertSnapshot::ErrCaptureNewPage(
        DBID dbid,
        PGNO pgno,
        RBS_POS *prbsposRecord )
{
    Assert( m_fInitialized );
    Assert( !m_fInvalid );

    DATA dataDummy;
    dataDummy.Nullify();

    RBSDbNewPageRecord dbRec;
    dbRec.m_bRecType = rbsrectypeDbNewPage;
    dbRec.m_usRecLength = sizeof( RBSDbNewPageRecord );
    dbRec.m_dbid = dbid;
    dbRec.m_pgno = pgno;

    return ErrCaptureRec( &dbRec, &dataDummy, prbsposRecord );
}

ERR CRevertSnapshot::ErrCaptureDbHeader( FMP * const pfmp )
{
    RBS_POS dummy;
    RBSDbHdrRecord dbRec;
    dbRec.m_bRecType = rbsrectypeDbHdr;
    dbRec.m_usRecLength = sizeof( RBSDbHdrRecord ) + sizeof( DBFILEHDR );
    dbRec.m_dbid = pfmp->Dbid();

    DATA dataRec;
    dataRec.SetPv( (VOID *)pfmp->Pdbfilehdr().get() );
    dataRec.SetCb( sizeof( DBFILEHDR ) );

    return ErrCaptureRec( &dbRec, &dataRec, &dummy );
}

ERR CRevertSnapshot::ErrCaptureDbAttach( FMP * const pfmp )
{
    RBS_POS dummy;

    DATA dataRec;
    dataRec.SetPv( pfmp->WszDatabaseName() );
    dataRec.SetCb( ( wcslen( pfmp->WszDatabaseName() ) + 1 ) * sizeof(WCHAR) );

    RBSDbAttachRecord dbRec;
    dbRec.m_bRecType = rbsrectypeDbAttach;
    dbRec.m_dbid = pfmp->Dbid();
    dbRec.m_usRecLength = sizeof( RBSDbAttachRecord ) + dataRec.Cb();

    return ErrCaptureRec( &dbRec, &dataRec, &dummy );
}

ERR CRevertSnapshot::ErrQueueCurrentAndAllocBuffer()
{
    ERR err;

    Assert( m_critBufferLock.FOwner() );

    if ( m_pActiveBuffer != NULL && m_pActiveBuffer->m_pBuffer != NULL )
    {
        Assert( m_pActiveBuffer->m_ibNextRecord >= cbRBSBufferSize );

        m_pActiveBuffer->m_cbValidData = cbRBSBufferSize;
        if ( m_pBuffersToWrite == NULL )
        {
            m_pBuffersToWrite = m_pBuffersToWriteLast = m_pActiveBuffer;
        }
        else
        {
            m_pBuffersToWriteLast->m_pNextBuffer = m_pActiveBuffer;
            m_pBuffersToWriteLast = m_pActiveBuffer;
        }
        Assert( m_pActiveBuffer->m_pNextBuffer == NULL );
        m_cNextActiveSegment += CsegRBSCountSegmentOfOffset( m_pActiveBuffer->m_cbValidData );
        m_pActiveBuffer = NULL;
        if ( !m_fWriteInProgress )
        {
            m_fWriteInProgress = fTrue;
            if ( m_pinst->Taskmgr().ErrTMPost( WriteBuffers_, this ) < JET_errSuccess )
            {
                m_fWriteInProgress = fFalse;
            }
        }

        if ( m_cNextActiveSegment > cRBSSegmentMax )
        {
            return ErrERRCheck( JET_errOutOfRBSSpace );
        }
    }
    if ( m_pActiveBuffer == NULL )
    {
        AllocR( m_pActiveBuffer = new (&m_cresRBSBuf) CSnapshotBuffer( m_cNextActiveSegment, &m_cresRBSBuf ) );
    }
    Assert( m_pActiveBuffer->m_pBuffer == NULL );
    return m_pActiveBuffer->ErrAllocBuffer();
}

ERR CRevertSnapshot::ErrCaptureRec(
        const RBSRecord * pRec,
        const DATA      * pExtraData,
              RBS_POS   * prbsposRecord )
{
    ERR err;

    CSnapshotBuffer::PreAllocReserveBuffer();

    ENTERCRITICALSECTION critBuf( &m_critBufferLock );

    Assert( FInitialized() );

    USHORT cbRec = CbRBSRecFixed( pRec->m_bRecType );
    Assert( pRec->m_usRecLength == cbRec + pExtraData->Cb() );
    ULONG cbRemaining = cbRec + pExtraData->Cb();
    BOOL fFirstPass = fTrue;

    do
    {
        if ( m_pActiveBuffer == NULL ||
             m_pActiveBuffer->m_pBuffer == NULL ||
             m_pActiveBuffer->m_ibNextRecord >= cbRBSBufferSize )
        {
            Call( ErrQueueCurrentAndAllocBuffer() );
        }

        Assert( m_pActiveBuffer->m_cStartSegment == m_cNextActiveSegment );

        ULONG cbSegmentSpaceRemaining = cbRBSSegmentSize - IbRBSSegmentOffsetFromFullOffset( m_pActiveBuffer->m_ibNextRecord );
        Assert( cbSegmentSpaceRemaining >= sizeof( RBSFragBegin ) );
        Assert( cbSegmentSpaceRemaining <= cbRBSSegmentSize - sizeof( RBSSEGHDR ) );

        if ( fFirstPass )
        {
            if ( cbSegmentSpaceRemaining >= cbRemaining )
            {
                UtilMemCpy( m_pActiveBuffer->m_pBuffer + m_pActiveBuffer->m_ibNextRecord, pRec, cbRec );
                UtilMemCpy( m_pActiveBuffer->m_pBuffer + m_pActiveBuffer->m_ibNextRecord + cbRec, pExtraData->Pv(), pExtraData->Cb() );
                m_pActiveBuffer->m_ibNextRecord += cbRemaining;
                cbSegmentSpaceRemaining -= cbRemaining;
                cbRemaining = 0;
            }
            else
            {
                RBSFragBegin fragBeginRec;
                fragBeginRec.m_bRecType = rbsrectypeFragBegin;
                fragBeginRec.m_usRecLength = (USHORT)cbSegmentSpaceRemaining;
                fragBeginRec.m_usTotalRecLength = (USHORT)cbRemaining;

                UtilMemCpy( m_pActiveBuffer->m_pBuffer + m_pActiveBuffer->m_ibNextRecord, &fragBeginRec, sizeof( RBSFragBegin ) );
                m_pActiveBuffer->m_ibNextRecord += sizeof( RBSFragBegin );
                cbSegmentSpaceRemaining -= sizeof( RBSFragBegin );
                if ( cbSegmentSpaceRemaining > 0 )
                {
                    ULONG cbToCopy = min( cbRec, cbSegmentSpaceRemaining );
                    UtilMemCpy( m_pActiveBuffer->m_pBuffer + m_pActiveBuffer->m_ibNextRecord, pRec, cbToCopy );
                    m_pActiveBuffer->m_ibNextRecord += cbToCopy;
                    cbSegmentSpaceRemaining -= cbToCopy;
                    cbRemaining -= cbToCopy;
                }
                if ( cbSegmentSpaceRemaining > 0 )
                {
                    ULONG cbToCopy = min( (ULONG)pExtraData->Cb(), cbSegmentSpaceRemaining );
                    UtilMemCpy( m_pActiveBuffer->m_pBuffer + m_pActiveBuffer->m_ibNextRecord, pExtraData->Pv(), cbToCopy );
                    m_pActiveBuffer->m_ibNextRecord += cbToCopy;
                    cbSegmentSpaceRemaining -= cbToCopy;
                    cbRemaining -= cbToCopy;
                }
                Assert( cbSegmentSpaceRemaining == 0 );
                Assert( cbRemaining > 0 );
            }
            fFirstPass = fFalse;
        }
        else
        {
            Assert( cbSegmentSpaceRemaining == cbRBSSegmentSize - sizeof ( RBSSEGHDR ) );

            RBSFragContinue fragContdRec;
            fragContdRec.m_bRecType = rbsrectypeFragContinue;
            fragContdRec.m_usRecLength = min( sizeof( RBSFragContinue ) + cbRemaining, cbSegmentSpaceRemaining );

            UtilMemCpy( m_pActiveBuffer->m_pBuffer + m_pActiveBuffer->m_ibNextRecord, &fragContdRec, sizeof( RBSFragContinue ) );
            m_pActiveBuffer->m_ibNextRecord += sizeof( RBSFragContinue );
            cbSegmentSpaceRemaining -= sizeof( RBSFragContinue );

            if ( cbRemaining > (ULONG)pExtraData->Cb() )
            {
                UtilMemCpy( m_pActiveBuffer->m_pBuffer + m_pActiveBuffer->m_ibNextRecord, (BYTE *)pRec + cbRec - ( cbRemaining - pExtraData->Cb() ), cbRemaining - pExtraData->Cb() );
                m_pActiveBuffer->m_ibNextRecord += cbRemaining - pExtraData->Cb();
                cbSegmentSpaceRemaining -= cbRemaining - pExtraData->Cb();
                cbRemaining = pExtraData->Cb();
            }
            ULONG cbToCopy = min( cbRemaining, cbSegmentSpaceRemaining );
            UtilMemCpy( m_pActiveBuffer->m_pBuffer + m_pActiveBuffer->m_ibNextRecord, (BYTE *)pExtraData->Pv() + pExtraData->Cb() - cbRemaining, cbToCopy );
            m_pActiveBuffer->m_ibNextRecord += cbToCopy;
            cbSegmentSpaceRemaining -= cbToCopy;
            cbRemaining -= cbToCopy;
        }

        if ( cbRemaining == 0 )
        {
            prbsposRecord->lGeneration = m_prbsfilehdrCurrent->rbsfilehdr.le_lGeneration;
            prbsposRecord->iSegment = m_pActiveBuffer->m_cStartSegment + CsegRBSCountSegmentOfOffset( m_pActiveBuffer->m_ibNextRecord + cbSegmentSpaceRemaining ) - 1;
        }

        if ( cbSegmentSpaceRemaining < sizeof( RBSFragBegin ) )
        {
            if ( cbSegmentSpaceRemaining > 0 )
            {
                memset( m_pActiveBuffer->m_pBuffer + m_pActiveBuffer->m_ibNextRecord, 0, cbSegmentSpaceRemaining );
            }
            RBSSEGHDR *pHdr = (RBSSEGHDR *)( m_pActiveBuffer->m_pBuffer + m_pActiveBuffer->m_ibNextRecord + cbSegmentSpaceRemaining - cbRBSSegmentSize );
            pHdr->le_iSegment = m_pActiveBuffer->m_cStartSegment + CsegRBSCountSegmentOfOffset((ULONG)( (BYTE *)pHdr - m_pActiveBuffer->m_pBuffer ));
            LGIGetDateTime( &pHdr->logtimeSegment );
            SetPageChecksum( pHdr, cbRBSSegmentSize, rbsPage, pHdr->le_iSegment );

            m_pActiveBuffer->m_ibNextRecord += cbSegmentSpaceRemaining + sizeof( RBSSEGHDR );
        }
    }
    while( cbRemaining > 0 );

    return JET_errSuccess;

HandleError:
    delete m_pActiveBuffer;
    m_pActiveBuffer = NULL;
    return err;
}

ERR CRevertSnapshot::ErrSetReadBuffer( ULONG iStartSegment )
{
    Assert( m_fInitialized );
    Assert( !m_fInvalid );

    if ( m_pReadBuffer != NULL )
    {
        delete m_pReadBuffer;
    }
    
    ERR err = JET_errSuccess;
    AllocR( m_pReadBuffer = new CSnapshotReadBuffer( iStartSegment ) );
    return err;
}

VOID CRevertSnapshot::LogCorruptionEvent( PCWSTR wszReason, __int64 checksumExpected, __int64 checksumActual )
{
    QWORD ibOffset = IbRBSFileOffsetOfSegment( m_pReadBuffer->m_cStartSegment ) + m_pReadBuffer->m_ibNextRecord;
    ULONG cbSegment = m_pReadBuffer->m_cStartSegment + CsegRBSCountSegmentOfOffset( m_pReadBuffer->m_ibNextRecord );

    WCHAR wszOffsetCurrent[64], wszSegmentCurrent[16], wszChecksumExpected[64], wszChecksumActual[64];
    OSStrCbFormatW( wszOffsetCurrent, sizeof(wszOffsetCurrent),  L"%I64u (0x%I64x)", ibOffset, ibOffset );
    OSStrCbFormatW( wszSegmentCurrent, sizeof(wszSegmentCurrent), L"%u", cbSegment );
    OSStrCbFormatW( wszChecksumExpected, sizeof(wszChecksumExpected), L"%I64u (0x%I64x)", checksumExpected, checksumExpected );
    OSStrCbFormatW( wszChecksumActual, sizeof(wszChecksumActual), L"%I64u (0x%I64x)", checksumActual, checksumActual );

    PCWSTR rgcwsz[6];
    rgcwsz[0] = m_wszRBSCurrentFile;
    rgcwsz[1] = wszReason;
    rgcwsz[2] = wszOffsetCurrent;
    rgcwsz[3] = wszSegmentCurrent;
    rgcwsz[4] = wszChecksumExpected;
    rgcwsz[5] = wszChecksumActual;

    UtilReportEvent(
            eventError,
            GENERAL_CATEGORY,
            RBS_CORRUPT_ID,
            6,
            rgcwsz,
            0,
            NULL,
            m_pinst );

    if ( m_fDumping )
    {
        DUMPPrintF( "!Corruption! - reason: %ws, Offset: %ws, Segment: %ws, Expected checksum: %ws, Actual checksum: %ws\n",
                wszReason, wszOffsetCurrent, wszSegmentCurrent, wszChecksumExpected, wszChecksumActual );
    }
}

ERR CRevertSnapshot::ErrGetNextRecord( RBSRecord **ppRecord, RBS_POS* rbsposRecStart, _Out_ PWSTR wszErrReason )
{
    Assert( m_fInitialized );
    Assert( !m_fInvalid );

    ERR err = JET_errSuccess;
    PCWSTR wszReason = L"Unknown";
    PAGECHECKSUM checksumExpected, checksumActual;
    
    BOOL fValidRecFound = fFalse;

    *ppRecord = NULL;
    rbsposRecStart->lGeneration = m_prbsfilehdrCurrent == NULL ? 0 : m_prbsfilehdrCurrent->rbsfilehdr.le_lGeneration;

    if ( m_pReadBuffer == NULL )
    {
        Alloc( m_pReadBuffer = new CSnapshotReadBuffer( 0 ) );
    }
    if ( m_pReadBuffer->m_pBuffer == NULL )
    {
        Call( m_pReadBuffer->ErrAllocBuffer() );
    }

    ULONG cbTotal =0, cbRemaining = 0;

    if ( IbRBSSegmentOffsetFromFullOffset( m_pReadBuffer->m_ibNextRecord ) != 0 &&
         *( m_pReadBuffer->m_pBuffer + m_pReadBuffer->m_ibNextRecord ) == rbsrectypeNOP )
    {
        m_pReadBuffer->m_ibNextRecord += cbRBSSegmentSize - IbRBSSegmentOffsetFromFullOffset( m_pReadBuffer->m_ibNextRecord );
    }

    do
    {
        if ( m_pReadBuffer->m_cStartSegment == 0 || m_pReadBuffer->m_ibNextRecord >= m_pReadBuffer->m_cbValidData )
        {
            m_pReadBuffer->m_cStartSegment = ( m_pReadBuffer->m_cStartSegment == 0 ) ? 1 : ( m_pReadBuffer->m_cStartSegment + CsegRBSCountSegmentOfOffset( m_pReadBuffer->m_cbValidData ) );
            m_pReadBuffer->m_ibNextRecord = 0;
            QWORD ibOffset = IbRBSFileOffsetOfSegment( m_pReadBuffer->m_cStartSegment );
            if ( ibOffset >= m_prbsfilehdrCurrent->rbsfilehdr.le_cbLogicalFileSize )
            {
                err = ErrERRCheck( JET_wrnNoMoreRecords );
                goto HandleError;
            }
            DWORD cbToRead = (DWORD)min( cbRBSBufferSize, m_prbsfilehdrCurrent->rbsfilehdr.le_cbLogicalFileSize - ibOffset );
            TraceContextScope tcScope( iorpRBS );
            Call( m_pfapiRBS->ErrIORead( *tcScope, ibOffset, cbToRead, m_pReadBuffer->m_pBuffer, QosSyncDefault( m_pinst ) ) );
            m_pReadBuffer->m_cbValidData = cbToRead;
        }

        if ( IbRBSSegmentOffsetFromFullOffset( m_pReadBuffer->m_ibNextRecord ) == 0 )
        {
            BOOL fCorrectableError;
            INT ibitCorrupted;
            ChecksumAndPossiblyFixPage(
                    m_pReadBuffer->m_pBuffer + m_pReadBuffer->m_ibNextRecord,
                    cbRBSSegmentSize,
                    rbsPage,
                    m_pReadBuffer->m_cStartSegment + CsegRBSCountSegmentOfOffset( m_pReadBuffer->m_ibNextRecord ),
                    fTrue,
                    &checksumExpected,
                    &checksumActual,
                    &fCorrectableError,
                    &ibitCorrupted );
            if ( checksumExpected != checksumActual )
            {
                wszReason = L"BadChecksum";
                err = ErrERRCheck( JET_errRBSFileCorrupt );
                goto HandleError;
            }
            RBSSEGHDR *pHdr = (RBSSEGHDR *)( m_pReadBuffer->m_pBuffer + m_pReadBuffer->m_ibNextRecord );
            if ( pHdr->le_iSegment != m_pReadBuffer->m_cStartSegment + CsegRBSCountSegmentOfOffset( m_pReadBuffer->m_ibNextRecord ) )
            {
                wszReason = L"BadSegmentNumber";
                err = ErrERRCheck( JET_errRBSFileCorrupt );
                goto HandleError;
            }

            if ( m_fDumping )
            {
                DUMPPrintF( "Segment:%u Checksum: 0x%016I64x, logtime ",
                        (SHORT)pHdr->le_iSegment,
                        (XECHECKSUM)pHdr->checksum );
                DUMPPrintLogTime( &pHdr->logtimeSegment );
                DUMPPrintF("\n");
            }

            m_pReadBuffer->m_ibNextRecord += sizeof( RBSSEGHDR );
        }

        RBSRecord *pRecord = (RBSRecord *)( m_pReadBuffer->m_pBuffer + m_pReadBuffer->m_ibNextRecord );
        if ( pRecord->m_usRecLength + IbRBSSegmentOffsetFromFullOffset( m_pReadBuffer->m_ibNextRecord ) > cbRBSSegmentSize )
        {
            wszReason = L"RecordTooLong";
            Assert( pRecord->m_usRecLength + IbRBSSegmentOffsetFromFullOffset( m_pReadBuffer->m_ibNextRecord ) <= cbRBSSegmentSize );
            err = ErrERRCheck( JET_errRBSFileCorrupt );
            goto HandleError;
        }
        switch ( pRecord->m_bRecType )
        {
            case rbsrectypeFragBegin:
                RBSFragBegin *pRecBegin;
                pRecBegin = (RBSFragBegin *)pRecord;
                cbRemaining = cbTotal = pRecBegin->m_usTotalRecLength;
                if ( m_pReadBuffer->m_cbAssembledRec < cbTotal )
                {
                    BYTE *pvRealloc;
                    Alloc( pvRealloc = (BYTE *)realloc( m_pReadBuffer->m_pvAssembledRec, cbTotal ) );
                    m_pReadBuffer->m_pvAssembledRec = pvRealloc;
                    m_pReadBuffer->m_cbAssembledRec = cbTotal;
                }
                Assert( cbTotal <= m_pReadBuffer->m_cbAssembledRec );
                if ( cbRemaining <= pRecord->m_usRecLength - sizeof( RBSFragBegin ) )
                {
                    wszReason = L"FragBeginTooLong";
                    Assert( cbRemaining > pRecord->m_usRecLength - sizeof( RBSFragBegin ) );
                    err = ErrERRCheck( JET_errRBSFileCorrupt );
                    goto HandleError;
                }
                UtilMemCpy( m_pReadBuffer->m_pvAssembledRec, pRecBegin + 1, pRecBegin->m_usRecLength - sizeof( RBSFragBegin ) );
                cbRemaining -= pRecBegin->m_usRecLength - sizeof( RBSFragBegin );
                fValidRecFound = fTrue;
                rbsposRecStart->iSegment = CsegRBSCountSegmentOfOffset( ( m_pReadBuffer->m_ibNextRecord + sizeof( RBSFragBegin ) ) ) + m_pReadBuffer->m_cStartSegment;

                Assert( cbRemaining != 0 );
                break;

            case rbsrectypeFragContinue:
                if ( fValidRecFound )
                {
                    RBSFragContinue *pRecContinue;
                    pRecContinue = (RBSFragContinue *)pRecord;
                    Assert( cbTotal <= m_pReadBuffer->m_cbAssembledRec );
                    if ( cbRemaining < pRecord->m_usRecLength - sizeof( RBSFragContinue ) )
                    {
                        wszReason = L"FragContinueTooLong";
                        Assert( cbRemaining >= pRecord->m_usRecLength - sizeof( RBSFragContinue ) );
                        err = ErrERRCheck( JET_errRBSFileCorrupt );
                        goto HandleError;
                    }
                    UtilMemCpy( m_pReadBuffer->m_pvAssembledRec + cbTotal - cbRemaining, pRecContinue + 1, pRecContinue->m_usRecLength - sizeof( RBSFragContinue ) );
                    cbRemaining -= pRecContinue->m_usRecLength - sizeof( RBSFragContinue );
                    if ( cbRemaining == 0 )
                    {
                        *ppRecord = (RBSRecord *)m_pReadBuffer->m_pvAssembledRec;
                    }
                }
                break;

            default:
                *ppRecord = pRecord;
                fValidRecFound = true;
                cbRemaining = 0;
                rbsposRecStart->iSegment = CsegRBSCountSegmentOfOffset( m_pReadBuffer->m_ibNextRecord ) + m_pReadBuffer->m_cStartSegment;
                break;
        }

        m_pReadBuffer->m_ibNextRecord += pRecord->m_usRecLength;
    }
    while ( cbRemaining > 0 || !fValidRecFound );

    if ( (*ppRecord)->m_bRecType >= rbsrectypeMax )
    {
        Assert( (*ppRecord)->m_bRecType == rbsrectypeDbAttach || (*ppRecord)->m_bRecType == rbsrectypeDbHdr || (*ppRecord)->m_bRecType == rbsrectypeDbAttach );
        wszReason = L"UnknownRecType";
        err = ErrERRCheck( JET_errRBSFileCorrupt );
    }

HandleError:
    if ( err == JET_errRBSFileCorrupt )
    {
        LogCorruptionEvent( wszReason, checksumExpected.rgChecksum[0], checksumActual.rgChecksum[0] );

        CallS( ErrOSStrCbCopyW( wszErrReason, cbOSFSAPI_MAX_PATHW, wszReason ) );

        Assert( fFalse );
    }
    return err;
}

DWORD CRevertSnapshot::WriteBuffers_( VOID *pvThis )
{
    CRevertSnapshot *pSnapshot = (CRevertSnapshot *)pvThis;
    (VOID)pSnapshot->ErrWriteBuffers();
    return 0;
}

ERR CRevertSnapshot::ErrWriteBuffers()
{
    ERR err = JET_errSuccess;
    CSnapshotBuffer *pBuffer, *pBufferToDelete = NULL;
    BOOL fLogSpaceUsage = fFalse;

    Assert( FInitialized() );
    Assert( !FInvalid() );

    {
    ENTERCRITICALSECTION critWrite( &m_critWriteLock );
    while ( fTrue )
    {
        pBuffer = m_pBuffersToWrite;
        if ( pBuffer == NULL )
        {
            ENTERCRITICALSECTION critBuf( &m_critBufferLock );
            pBuffer = m_pBuffersToWrite;
            if ( pBuffer == NULL )
            {
                m_fWriteInProgress = fFalse;
                break;
            }
        }

        Assert( pBuffer == m_pBuffersToWrite );
        Assert( pBuffer->m_cStartSegment == m_cNextWriteSegment );
        QWORD ibOffset = IbRBSFileOffsetOfSegment( pBuffer->m_cStartSegment );
        QWORD ibOffsetEnd = ibOffset + pBuffer->m_cbValidData;
        QWORD cbFileSize;
        Call( m_pfapiRBS->ErrSize( &cbFileSize, IFileAPI::filesizeLogical ) );
        if ( ibOffsetEnd > cbFileSize )
        {
            TraceContextScope tcHeader( iorpRBS, iorsHeader );
            ULONG cbExtensionSize = (ULONG) max( UlParam( m_pinst, JET_paramDbExtensionSize ), cbRBSBufferSize );
            Call( m_pfapiRBS->ErrSetSize( *tcHeader, cbFileSize + cbExtensionSize, fFalse, qosIONormal ) );
        }
        TraceContextScope tcScope( iorpRBS );
        Call( m_pfapiRBS->ErrIOWrite( *tcScope, ibOffset, pBuffer->m_cbValidData, pBuffer->m_pBuffer, QosSyncDefault( m_pinst ) ) );

        {
        ENTERCRITICALSECTION critBuf( &m_critBufferLock );
        Assert( pBuffer == m_pBuffersToWrite );
        m_pBuffersToWrite = pBuffer->m_pNextBuffer;
        m_cNextWriteSegment += CsegRBSCountSegmentOfOffset( pBuffer->m_cbValidData );
        OSTrace( JET_tracetagRBS, OSFormat( "RBS write position:%u,%u\n", (LONG) m_prbsfilehdrCurrent->rbsfilehdr.le_lGeneration, m_cNextWriteSegment ) );
        pBufferToDelete = pBuffer;

        if ( m_pBuffersToWrite == NULL )
        {
            m_pBuffersToWriteLast = NULL;
            m_fWriteInProgress = fFalse;
            break;
        }
        }

        delete pBufferToDelete;
        pBufferToDelete = NULL;
    }

    delete pBufferToDelete;
    pBufferToDelete = NULL;

    if ( m_cNextFlushSegment - m_cNextWriteSegment >= ( cbRBSSegmentsInBuffer ) * 2 )
    {
        Call( ErrFlush() );
        fLogSpaceUsage = fTrue;
    }
    }

    if ( fLogSpaceUsage )
    {
        RBSLogSpaceUsage();
    }

HandleError:
    return err;
}

ERR CRevertSnapshot::ErrFlush()
{
    ERR err = JET_errSuccess;

    Assert( FInitialized() );
    Assert( m_critWriteLock.FOwner() );

    ULONG cNextWriteSegment = m_cNextWriteSegment;
    if ( m_cNextFlushSegment < cNextWriteSegment )
    {
        Call( ErrUtilFlushFileBuffers( m_pfapiRBS, iofrRBS ) );

        m_prbsfilehdrCurrent->rbsfilehdr.le_cbLogicalFileSize = IbRBSFileOffsetOfSegment( cNextWriteSegment );
        
        Call( ErrUtilWriteRBSHeaders( m_pinst, m_pinst->m_pfsapi, NULL, m_prbsfilehdrCurrent, m_pfapiRBS ) );

        m_cNextFlushSegment = cNextWriteSegment;
        m_tickLastFlush = TickOSTimeCurrent();
        OSTrace( JET_tracetagRBS, OSFormat("RBS flush position:%u,%u\n", (LONG)m_prbsfilehdrCurrent->rbsfilehdr.le_lGeneration, m_cNextFlushSegment ) );
    }

HandleError:
    return err;
}

ERR CRevertSnapshot::ErrFlushAll()
{
    ERR err;
    Assert( m_fInitialized );
    Assert( !m_fInvalid );

    if ( m_pActiveBuffer != NULL )
    {
        ENTERCRITICALSECTION critBuf( &m_critBufferLock );

        if ( m_pActiveBuffer != NULL )
        {
            ULONG ibOffset = IbRBSSegmentOffsetFromFullOffset( m_pActiveBuffer->m_ibNextRecord );
            Assert( ibOffset >= sizeof( RBSSEGHDR ) );
            if ( ibOffset == sizeof( RBSSEGHDR ) )
            {
                m_pActiveBuffer->m_cbValidData = m_pActiveBuffer->m_ibNextRecord - ibOffset;
            }
            else
            {
                RBSSEGHDR *pHdr = (RBSSEGHDR *)( m_pActiveBuffer->m_pBuffer + m_pActiveBuffer->m_ibNextRecord - ibOffset );
                memset( (BYTE *)pHdr + ibOffset, 0, cbRBSSegmentSize - ibOffset );
                pHdr->le_iSegment = m_pActiveBuffer->m_cStartSegment + CsegRBSCountSegmentOfOffset((ULONG)( (BYTE *)pHdr - m_pActiveBuffer->m_pBuffer ));
                LGIGetDateTime( &pHdr->logtimeSegment );
                SetPageChecksum( pHdr, cbRBSSegmentSize, rbsPage, pHdr->le_iSegment );

                m_pActiveBuffer->m_cbValidData = m_pActiveBuffer->m_ibNextRecord - ibOffset + cbRBSSegmentSize;
            }

            if ( m_pActiveBuffer->m_cbValidData > 0 )
            {
                if ( m_pBuffersToWrite == NULL )
                {
                    m_pBuffersToWrite = m_pBuffersToWriteLast = m_pActiveBuffer;
                }
                else
                {
                    m_pBuffersToWriteLast->m_pNextBuffer = m_pActiveBuffer;
                    m_pBuffersToWriteLast = m_pActiveBuffer;
                }
                Assert( m_pActiveBuffer->m_pNextBuffer == NULL );
                m_cNextActiveSegment += CsegRBSCountSegmentOfOffset( m_pActiveBuffer->m_cbValidData );
                m_pActiveBuffer = NULL;
            }
        }
    }

    Call( ErrWriteBuffers() );

    {
    ENTERCRITICALSECTION critWrite( &m_critWriteLock );
    Call( ErrFlush() );
    }

    RBSLogSpaceUsage();

HandleError:
    return err;
}

ERR CRevertSnapshot::ErrRBSSetRequiredLogs( BOOL fInferFromRstmap )
{
    if ( !m_fInitialized || 
        m_prbsfilehdrCurrent->rbsfilehdr.bLogsCopied || 
        m_prbsfilehdrCurrent->rbsfilehdr.le_lGenMinLogCopied > 0 )
    {
        return JET_errSuccess;
    }

    LONG lgenLow = 0;
    LONG lgenHigh = 0;

    if ( fInferFromRstmap )
    {
        m_pinst->m_plog->LoadRBSGenerationFromRstmap( &lgenLow, &lgenHigh );
    }
    else
    {
        RBSLoadRequiredGenerationFromFMP( m_pinst, &lgenLow, &lgenHigh );
    }

    m_prbsfilehdrCurrent->rbsfilehdr.le_lGenMinLogCopied = lgenLow;
    m_prbsfilehdrCurrent->rbsfilehdr.le_lGenMaxLogCopied = lgenHigh;
    return ErrUtilWriteRBSHeaders( m_pinst, m_pinst->m_pfsapi, NULL, m_prbsfilehdrCurrent, m_pfapiRBS );
}

ERR CRevertSnapshot::ErrRBSCopyRequiredLogs( BOOL fInferFromRstmap )
{
    if ( !m_fInitialized || 
        m_prbsfilehdrCurrent->rbsfilehdr.bLogsCopied )
    {
        return JET_errSuccess;
    }

    JET_ERR err = JET_errSuccess;

    if ( m_prbsfilehdrCurrent->rbsfilehdr.le_lGenMinLogCopied )
    {
        goto CopyLogs;
    }

    Call( ErrRBSSetRequiredLogs( fInferFromRstmap ) );

CopyLogs:
    Call( ErrRBSCopyRequiredLogs_( 
        m_pinst,
        m_prbsfilehdrCurrent->rbsfilehdr.le_lGenMinLogCopied, 
        m_prbsfilehdrCurrent->rbsfilehdr.le_lGenMaxLogCopied, 
        SzParam( m_pinst, JET_paramLogFilePath ), 
        m_wszRBSCurrentLogDir,
        fTrue,
        fTrue ) );

    m_prbsfilehdrCurrent->rbsfilehdr.bLogsCopied = 1;
    Call( ErrUtilWriteRBSHeaders( m_pinst, m_pinst->m_pfsapi, NULL, m_prbsfilehdrCurrent, m_pfapiRBS ) );

HandleError:
    return err;
}

ERR CRevertSnapshot::ErrRollSnapshot( BOOL fPrevRBSValid, BOOL fInferFromRstmap )
{
    Assert( m_prbsfilehdrCurrent );

    WCHAR   wszRBSAbsFilePath[ IFileSystemAPI::cchPathMax ];
    WCHAR   wszRBSAbsLogDirPath[ IFileSystemAPI::cchPathMax ];
    JET_ERR err = JET_errSuccess;

    RBSFILEHDR* prbsfilehdrPrev = m_prbsfilehdrCurrent;
    LOGTIME defaultLogTime      = { 0 };
    LOGTIME logtimePrevGen      = fPrevRBSValid ? prbsfilehdrPrev->rbsfilehdr.tmCreate : defaultLogTime;
    LONG    lRBSGen             = prbsfilehdrPrev->rbsfilehdr.le_lGeneration + 1;


    if ( fPrevRBSValid )
    {
        Call( ErrFlushAll() );
    }

    Alloc( m_prbsfilehdrCurrent = (RBSFILEHDR *)PvOSMemoryPageAlloc( sizeof(RBSFILEHDR), NULL ) );

    Call( ErrRBSRollAttachInfos( m_pinst, m_prbsfilehdrCurrent, prbsfilehdrPrev, fPrevRBSValid, fInferFromRstmap ) );


    delete m_pfapiRBS;
    m_pfapiRBS = NULL;
    FreeCurrentFilePath();
    FreeCurrentLogDirPath();
    OSMemoryPageFree( prbsfilehdrPrev );
    prbsfilehdrPrev = NULL;

    Call( ErrRBSCreateOrLoadRbsGen( 
        lRBSGen,
        logtimePrevGen, 
        wszRBSAbsFilePath, 
        wszRBSAbsLogDirPath ) );

    const SIZE_T    cchRBSName       = sizeof( WCHAR ) * ( LOSStrLengthW( wszRBSAbsFilePath ) + 1 );
    Alloc( m_wszRBSCurrentFile = static_cast<WCHAR *>( PvOSMemoryHeapAlloc( cchRBSName ) ) );

    const SIZE_T    cchRBSLogDirName       = sizeof( WCHAR ) * ( LOSStrLengthW( wszRBSAbsLogDirPath ) + 1 );
    Alloc( m_wszRBSCurrentLogDir = static_cast<WCHAR *>( PvOSMemoryHeapAlloc( cchRBSLogDirName ) ) );

    Call( ErrOSStrCbCopyW( m_wszRBSCurrentFile, cchRBSName, wszRBSAbsFilePath ) );
    Call( ErrOSStrCbCopyW( m_wszRBSCurrentLogDir, cchRBSLogDirName, wszRBSAbsLogDirPath ) );

    Call( ErrRBSCopyRequiredLogs( fInferFromRstmap ) );

    m_ftSpaceUsageLastLogged = 0;

HandleError:
    return err;
}

BOOL CRevertSnapshot::FRollSnapshot()
{
    if ( !FInitialized() || FInvalid() || m_prbsfilehdrCurrent == NULL )
    {
        return fFalse;
    }

    const __int64 cSecSinceFileCreate = UtilConvertFileTimeToSeconds( ConvertLogTimeToFileTime( &( m_prbsfilehdrCurrent->rbsfilehdr.tmCreate ) ) );
    const __int64 cSecCurrentTime = UtilConvertFileTimeToSeconds( UtilGetCurrentFileTime( ) );


    if( cSecCurrentTime - cSecSinceFileCreate > ( (ULONG) UlParam( m_pinst, JET_paramFlight_RBSRollIntervalSec ) ) )
    {
        return fTrue;
    }

    return fFalse;
}


ERR RBSCleanerFactory::ErrRBSCleanerCreate( INST*  pinst, __out RBSCleaner ** prbscleaner)
{
     ERR err = JET_errSuccess;

    if ( pinst && prbscleaner )
    {
        unique_ptr<RBSCleanerIOOperator> prbscleaneriooperator( new RBSCleanerIOOperator( pinst ) );
        unique_ptr<RBSCleanerState> prbscleanerstate( new RBSCleanerState() );
        unique_ptr<RBSCleanerConfig> prbscleanerconfig( new RBSCleanerConfig( pinst ) );

        Alloc( prbscleaneriooperator.get() );
        Alloc( prbscleanerstate.get() );
        Alloc( prbscleanerconfig.get() );

        Call( prbscleaneriooperator->ErrRBSInitPaths() );

        *prbscleaner = new RBSCleaner( pinst, prbscleaneriooperator.release(), prbscleanerstate.release(), prbscleanerconfig.release() );
        Alloc( *prbscleaner );
    }

    return JET_errSuccess;

HandleError:
    return err;
}

RBSCleanerState::RBSCleanerState() :
    m_ftPrevPassCompletionTime( 0 ),
    m_ftPassStartTime( 0 ),
    m_cPassesFinished( 0 )
{
}

RBSCleanerIOOperator::RBSCleanerIOOperator( INST* pinst )
{
    Assert( pinst );
    m_pinst = pinst;
    m_wszRBSAbsRootDirPath = NULL;
    m_wszRBSBaseName = NULL;
}

RBSCleanerIOOperator::~RBSCleanerIOOperator( )
{
    if ( m_wszRBSAbsRootDirPath )
    {
        OSMemoryHeapFree( m_wszRBSAbsRootDirPath );
        m_wszRBSAbsRootDirPath = NULL;
    }

    if ( m_wszRBSBaseName )
    {
        OSMemoryHeapFree( m_wszRBSBaseName );
        m_wszRBSBaseName = NULL;
    }
}

ERR RBSCleanerIOOperator::ErrRBSInitPaths()
{
    return ErrRBSInitPaths_( m_pinst, &m_wszRBSAbsRootDirPath, &m_wszRBSBaseName );
}

ERR RBSCleanerIOOperator::ErrRBSDiskSpace( QWORD* pcbFreeForUser )
{
    Assert( m_pinst->m_pfsapi );
    return m_pinst->m_pfsapi->ErrDiskSpace( m_wszRBSAbsRootDirPath, pcbFreeForUser );
}

ERR RBSCleanerIOOperator::ErrGetDirSize( PCWSTR wszDirPath, _Out_ QWORD* pcbSize )
{
    Assert( m_pinst->m_pfsapi );
    return ErrRBSGetDirSize( m_pinst->m_pfsapi, m_wszRBSAbsRootDirPath, pcbSize );
}

ERR RBSCleanerIOOperator::ErrRemoveFolder( PCWSTR wszDirPath, PCWSTR wszRBSRemoveReason )
{
    Assert( m_pinst->m_pfsapi );

    ERR err = JET_errSuccess;
    Call( ErrRBSDeleteAllFiles( m_pinst->m_pfsapi, wszDirPath, NULL, fTrue ) );

    PCWSTR rgcwsz[2];
    rgcwsz[0] = wszDirPath;
    rgcwsz[1] = wszRBSRemoveReason;

    UtilReportEvent(
            eventInformation,
            GENERAL_CATEGORY,
            RBSCLEANER_REMOVEDRBS_ID,
            2,
            rgcwsz,
            0,
            NULL,
            m_pinst );
HandleError:
    return err;
}

ERR RBSCleanerIOOperator::ErrRBSGetLowestAndHighestGen( LONG* plRBSGenMin, LONG* plRBSGenMax )
{
    Assert( m_pinst->m_pfsapi );
    return ErrRBSGetLowestAndHighestGen_( m_pinst->m_pfsapi, m_wszRBSAbsRootDirPath, m_wszRBSBaseName, plRBSGenMin, plRBSGenMax );
}

ERR RBSCleanerIOOperator::ErrRBSFilePathForGen( __out_bcount ( cbDirPath ) WCHAR* wszRBSDirPath, LONG cbDirPath, __out_bcount ( cbFilePath ) WCHAR* wszRBSFilePath, LONG cbFilePath, LONG lRBSGen )
{
    Assert( m_pinst->m_pfsapi );
    return ErrRBSFilePathForGen_( m_wszRBSAbsRootDirPath, m_wszRBSBaseName, m_pinst->m_pfsapi, wszRBSDirPath, cbDirPath, wszRBSFilePath, cbFilePath, lRBSGen );
}

ERR RBSCleanerIOOperator::ErrRBSFileHeader( PCWSTR wszRBSFilePath, _Out_ RBSFILEHDR* prbsfilehdr )
{
    Assert( wszRBSFilePath );
    Assert( prbsfilehdr );

    ERR             err         = JET_errSuccess;
    IFileAPI        *pfapiRBS   = NULL;
    IFileSystemAPI  *pfsapi  = m_pinst->m_pfsapi;

    Assert( pfsapi );

    Call( CIOFilePerf::ErrFileOpen( pfsapi, m_pinst, wszRBSFilePath, IFileAPI::fmfReadOnly, iofileRBS, qwRBSFileID, &pfapiRBS ) );
    Call( ErrUtilReadShadowedHeader( m_pinst, pfsapi, pfapiRBS, (BYTE*) prbsfilehdr, sizeof( RBSFILEHDR ), -1, urhfNoAutoDetectPageSize | urhfReadOnly | urhfNoEventLogging ) );

HandleError:
    if ( pfapiRBS )
    {
        delete pfapiRBS;
        pfapiRBS = NULL;
    }

    return err;
}

RBSCleaner::RBSCleaner( 
    INST*                           pinst,
    IRBSCleanerIOOperator* const    prbscleaneriooperator,
    IRBSCleanerState* const         prbscleanerstate,
    IRBSCleanerConfig* const        prbscleanerconfig ) : 
    CZeroInit( sizeof( RBSCleaner ) ),
    m_pinst( pinst ),
    m_msigRBSCleanerStop( CSyncBasicInfo( _T("RBSCleaner::m_msigRBSCleanerStop" ) ) ),
    m_critRBSFirstValidGen( CLockBasicInfo( CSyncBasicInfo( szRBSFirstValidGen ), rankRBSFirstValidGen, 0 ) ),
    m_prbscleaneriooperator( prbscleaneriooperator ),
    m_prbscleanerstate( prbscleanerstate ),
    m_prbscleanerconfig( prbscleanerconfig ),
    m_threadRBSCleaner( 0 ),
    m_lFirstValidRBSGen( 1 ),
    m_fValidRBSGenSet( fFalse )
{
    Assert( pinst );
    Assert( prbscleanerconfig );
    Assert( prbscleaneriooperator );
    Assert( prbscleanerstate );
}

RBSCleaner::~RBSCleaner( )
{
    TermCleaner( );
}

DWORD RBSCleaner::DwRBSCleanerThreadProc( DWORD_PTR dwContext )
{
    RBSCleaner * const prbscleaner = reinterpret_cast<RBSCleaner *>( dwContext );
    Assert( NULL != prbscleaner );
    return prbscleaner->DwRBSCleaner();
}

DWORD RBSCleaner::DwRBSCleaner()
{
    WaitForMinPassTime();
    ComputeFirstValidRBSGen();
    while ( !m_msigRBSCleanerStop.FIsSet() )
    {
        ErrDoOneCleanupPass();
        m_prbscleanerstate->CompletedPass();

        if ( m_msigRBSCleanerStop.FIsSet() || FMaxPassesReached() )
        {
            break;
        }

        WaitForMinPassTime();
    }

    return 0;
}

void RBSCleaner::WaitForMinPassTime()
{
    const __int64 ftNow = UtilGetCurrentFileTime();
    const __int64 csecSincePrevScanCompleted = UtilConvertFileTimeToSeconds( ftNow - m_prbscleanerstate->FtPrevPassCompletionTime() );
    const __int64 csecBeforeNextPassCanStart = m_prbscleanerconfig->CSecMinCleanupIntervalTime() - csecSincePrevScanCompleted;

    if ( csecBeforeNextPassCanStart > 0 )
    {
        Assert( csecBeforeNextPassCanStart <= ( INT_MAX / 1000 ) );
        ( void )m_msigRBSCleanerStop.FWait( -1000 * ( INT )csecBeforeNextPassCanStart );
    }
}

VOID RBSCleaner::ComputeFirstValidRBSGen()
{
    Assert( m_pinst );

    WCHAR       wszRBSAbsDirPath[ IFileSystemAPI::cchPathMax ];
    WCHAR       wszRBSAbsFilePath[ IFileSystemAPI::cchPathMax ];
    LONG        lRBSGenMax;
    LONG        lRBSGenMin;
    LOGTIME     tmPrevRBSCreate;
    RBSFILEHDR  rbsfilehdr;
    ERR         err = JET_errSuccess;

    if ( m_fValidRBSGenSet )
    {
        return;
    }

    Call( m_prbscleaneriooperator->ErrRBSGetLowestAndHighestGen( &lRBSGenMin, &lRBSGenMax ) );

    lRBSGenMax = BoolParam( m_pinst, JET_paramEnableRBS ) ? lRBSGenMax - 1 : lRBSGenMax;

    for ( LONG lRBSGen = lRBSGenMax; lRBSGen >= lRBSGenMin && !m_fValidRBSGenSet; --lRBSGen )
    {
        Call( m_prbscleaneriooperator->ErrRBSFilePathForGen( wszRBSAbsDirPath, sizeof( wszRBSAbsDirPath ), wszRBSAbsFilePath, sizeof( wszRBSAbsFilePath ), lRBSGen ) );
        Call( m_prbscleaneriooperator->ErrRBSFileHeader( wszRBSAbsFilePath, &rbsfilehdr ) );

        if ( tmPrevRBSCreate.FIsSet() && LOGTIME::CmpLogTime( rbsfilehdr.rbsfilehdr.tmCreate, tmPrevRBSCreate ) != 0 )
        {
            SetFirstValidGen( lRBSGen );
            return;
        }

        memcpy( &tmPrevRBSCreate, &rbsfilehdr.rbsfilehdr.tmPrevGen, sizeof( LOGTIME ) );

        if ( !tmPrevRBSCreate.FIsSet() )
        {
            SetFirstValidGen( lRBSGen );
            return;
        }
    }

HandleError:
    return;
}

ERR RBSCleaner::ErrDoOneCleanupPass()
{
    QWORD       cbFreeRBSDisk;
    QWORD       cbTotalRBSDiskSpace;
    ERR         err                         = JET_errSuccess;
    QWORD       cbMaxRBSSpaceLowDiskSpace   = m_prbscleanerconfig->CbMaxSpaceForRBSWhenLowDiskSpace();
    QWORD       cbLowDiskSpace              = m_prbscleanerconfig->CbLowDiskSpaceThreshold();

    LONG        lRBSGenMax;
    LONG        lRBSGenMin;

    m_prbscleanerstate->SetPassStartTime();

    Call( m_prbscleaneriooperator->ErrRBSDiskSpace( &cbFreeRBSDisk ) );

    Call( m_prbscleaneriooperator->ErrGetDirSize( m_prbscleaneriooperator->WSZRBSAbsRootDirPath(), &cbTotalRBSDiskSpace ) );

    while ( !m_msigRBSCleanerStop.FIsSet( ) )
    {
        WCHAR       wszRBSAbsDirPath[ IFileSystemAPI::cchPathMax ];
        WCHAR       wszRBSAbsFilePath[ IFileSystemAPI::cchPathMax ];
        PCWSTR      wszRBSRemoveReason  = L"Unknown";
        BOOL        fRBSCleanupMinGen   = fFalse;
        QWORD       cbRBSDiskSpace      = 0;
        __int64     ftCreate            = 0;
        RBSFILEHDR  rbsfilehdr;

        Call( m_prbscleaneriooperator->ErrRBSGetLowestAndHighestGen( &lRBSGenMin, &lRBSGenMax ) );
        

        if ( lRBSGenMax == 0 && !BoolParam( m_pinst, JET_paramEnableRBS ) )
        {
            m_msigRBSCleanerStop.Set();
            goto HandleError;
        }

        if ( lRBSGenMin != lRBSGenMax )
        {
            fRBSCleanupMinGen = !FGenValid( lRBSGenMin );
            Call( m_prbscleaneriooperator->ErrRBSFilePathForGen( wszRBSAbsDirPath, sizeof( wszRBSAbsDirPath ), wszRBSAbsFilePath, sizeof( wszRBSAbsFilePath ), lRBSGenMin ) );

            if ( !fRBSCleanupMinGen )
            {
                err = m_prbscleaneriooperator->ErrRBSFileHeader( wszRBSAbsFilePath, &rbsfilehdr );

                if ( err != JET_errFileNotFound && err != JET_errSuccess && err != JET_errReadVerifyFailure)
                {
                    goto HandleError;
                }

                if( err == JET_errSuccess ||  err == JET_errReadVerifyFailure )
                {
                    if ( err == JET_errReadVerifyFailure )
                    {
                        fRBSCleanupMinGen = fTrue;
                        wszRBSRemoveReason = L"CorruptHeader";
                    }
                    else
                    {
                        ftCreate = ConvertLogTimeToFileTime( &(rbsfilehdr.rbsfilehdr.tmCreate) );

                        if ( UtilConvertFileTimeToSeconds( UtilGetCurrentFileTime() - ftCreate ) > m_prbscleanerconfig->CSecRBSMaxTimeSpan() )
                        {
                            fRBSCleanupMinGen = fTrue;
                            wszRBSRemoveReason = L"Scavenged";
                        }
                    }
                }
                else
                {
                    fRBSCleanupMinGen = fTrue;
                    wszRBSRemoveReason = L"RBSFileMissing";
                }
            }
            else
            {
                wszRBSRemoveReason = L"InvalidRBS";
            }

            if ( fRBSCleanupMinGen || ( cbFreeRBSDisk < cbLowDiskSpace && cbTotalRBSDiskSpace > cbMaxRBSSpaceLowDiskSpace ) )
            {
                if ( !fRBSCleanupMinGen )
                {
                    wszRBSRemoveReason = L"LowDiskSpace";
                }

                Call( m_prbscleaneriooperator->ErrGetDirSize( wszRBSAbsDirPath, &cbRBSDiskSpace ) );
                Call( m_prbscleaneriooperator->ErrRemoveFolder( wszRBSAbsDirPath, wszRBSRemoveReason ) );
                cbTotalRBSDiskSpace -= cbRBSDiskSpace;
                cbFreeRBSDisk       += cbRBSDiskSpace;
            }
            else
            {
                goto HandleError;
            }
        }
        else
        {
            break;
        }
    }

HandleError:
    
    if ( err == JET_errFileNotFound && !BoolParam( m_pinst, JET_paramEnableRBS ) )
    {
        m_msigRBSCleanerStop.Set();
    }

    return err;
}

ERR RBSCleaner::ErrStartCleaner( )
{
    ERR err = JET_errSuccess;

    if ( !m_prbscleanerconfig->FEnableCleanup( ) )
    {
        return err;
    }

    Assert( !FIsCleanerRunning() );
    Call( ErrUtilThreadCreate( DwRBSCleanerThreadProc, 0, priorityNormal, &m_threadRBSCleaner, ( DWORD_PTR )this ) );
    Assert( FIsCleanerRunning() );

HandleError:
    if ( err < JET_errSuccess )
    {
        TermCleaner();
    }

    return err;
}

VOID RBSCleaner::TermCleaner()
{
    m_msigRBSCleanerStop.Set();

    if ( m_threadRBSCleaner != NULL )
    {
        UtilThreadEnd( m_threadRBSCleaner );
    }
    m_threadRBSCleaner = NULL;
  
    Assert( !FIsCleanerRunning() );
}


CRBSDatabaseRevertContext::CRBSDatabaseRevertContext( __in INST* const pinst )
    : CZeroInit( sizeof( CRBSDatabaseRevertContext ) ),
    m_pinst ( pinst ),
    m_dbidCurrent ( dbidMax ),
    m_asigWritePossible( CSyncBasicInfo( _T( "CRBSDatabaseRevertContext::m_asigWritePossible" ) ) )
{
    Assert( pinst );
}

CRBSDatabaseRevertContext::~CRBSDatabaseRevertContext()
{
    if ( m_wszDatabaseName )
    {
        OSMemoryHeapFree( m_wszDatabaseName );
        m_wszDatabaseName = NULL;
    }

    if ( m_pdbfilehdr )
    {
        OSMemoryPageFree( m_pdbfilehdr );
        m_pdbfilehdr = NULL;
    }

    if ( m_pdbfilehdrFromRBS )
    {
        OSMemoryPageFree( m_pdbfilehdrFromRBS );
        m_pdbfilehdrFromRBS = NULL;
    }

    if ( m_pfapiDb )
    {
        delete m_pfapiDb;
        m_pfapiDb = NULL;
    }

    if ( m_psbmDbPages )
    {
        delete m_psbmDbPages;
        m_psbmDbPages = NULL;
    }

    if ( m_pfm )
    {
        delete m_pfm;
        m_pfm = NULL;
    }

    if ( m_rgRBSDbPage )
    {
        CPG cpgTotal                            = m_rgRBSDbPage->Size();
        CArray< CPagePointer >::ERR errArray    = CArray< CPagePointer >::ERR::errSuccess;

        for ( int i = 0; i < cpgTotal; ++i )
        {
            if ( m_rgRBSDbPage->Entry( i ).DwPage() )
            {
                OSMemoryPageFree( (void*) m_rgRBSDbPage->Entry( i ).DwPage() );
            }

            errArray = m_rgRBSDbPage->ErrSetEntry( i, NULL );
            Assert( errArray == CArray< CPagePointer >::ERR::errSuccess );
        }

        delete m_rgRBSDbPage;
        m_rgRBSDbPage = NULL;
    }

    if ( m_pcprintfIRSTrace != NULL &&
        CPRINTFNULL::PcprintfInstance() != m_pcprintfIRSTrace )
    {
        (*m_pcprintfIRSTrace)( "Closing IRS tracing file from revert.\r\n" );
        delete m_pcprintfIRSTrace;
    }

    m_pcprintfIRSTrace = NULL;
}

ERR CRBSDatabaseRevertContext::ErrResetSbmDbPages()
{
    ERR err = JET_errSuccess;
    IBitmapAPI::ERR errBM = IBitmapAPI::ERR::errSuccess;

    if ( m_psbmDbPages )
    {
        delete m_psbmDbPages;
        m_psbmDbPages = NULL;
    }

    Alloc( m_psbmDbPages = new CSparseBitmap() );
    errBM = m_psbmDbPages->ErrInitBitmap( pgnoSysMax );

    if ( errBM != IBitmapAPI::ERR::errSuccess )
    {
        Assert( errBM == IBitmapAPI::ERR::errOutOfMemory );
        Call( ErrERRCheck( JET_errOutOfMemory ) );
    }

HandleError:
    return err;
}

ERR CRBSDatabaseRevertContext::ErrRBSDBRCInit( RBSATTACHINFO* prbsattachinfo, SIGNATURE* psignRBSHdrFlush, CPG cacheSize )
{
    ERR     err = JET_errSuccess;
    CAutoWSZ wszDatabaseName;
    CallR( wszDatabaseName.ErrSet( prbsattachinfo->wszDatabaseName ) );
    m_wszDatabaseName = static_cast<WCHAR *>( PvOSMemoryHeapAlloc( wszDatabaseName.Cb() + 2 ) );

    Alloc( m_wszDatabaseName );
    Call( ErrOSStrCbCopyW( m_wszDatabaseName, wszDatabaseName.Cb() + 2, wszDatabaseName ) );

    Call( m_pinst->m_pfsapi->ErrFileOpen( 
                m_wszDatabaseName,  
                BoolParam( JET_paramEnableFileCache ) ? IFileAPI::fmfCached : IFileAPI::fmfNone, 
                &m_pfapiDb ) );

    Alloc( m_pdbfilehdr = (DBFILEHDR*)PvOSMemoryPageAlloc( g_cbPage, NULL ) );

    err = ErrUtilReadShadowedHeader(    
            m_pinst,
            m_pinst->m_pfsapi,
            m_pfapiDb,
            (BYTE*)m_pdbfilehdr,
            g_cbPage,
            OffsetOf( DBFILEHDR, le_cbPageSize ) );

    if ( FErrIsDbHeaderCorruption( err ) || JET_errFileIOBeyondEOF == err )
    {
        err = ErrERRCheck( JET_errDatabaseCorrupted );
    }

    Call( err );

    if ( memcmp( &m_pdbfilehdr->signDb, &prbsattachinfo->signDb, sizeof( SIGNATURE ) ) != 0 ||
         !FRBSCheckForDbConsistency( &m_pdbfilehdr->signDbHdrFlush, &m_pdbfilehdr->signRBSHdrFlush, &prbsattachinfo->signDbHdrFlush,  psignRBSHdrFlush ) )
    {
        Error( ErrERRCheck( JET_errRBSRCInvalidRBS ) );
    }

    if ( m_pdbfilehdr->Dbstate() != JET_dbstateRevertInProgress &&
        ErrDBFormatFeatureEnabled_( JET_efvApplyRevertSnapshot, m_pdbfilehdr->Dbv() ) < JET_errSuccess )
    {
        Error( ErrERRCheck( JET_errRBSRCInvalidDbFormatVersion ) );
    }

    Alloc( m_rgRBSDbPage = new CArray< CPagePointer >() );
    m_rgRBSDbPage->ErrSetCapacity( (size_t) cacheSize );

    Call( ErrResetSbmDbPages() ); 

    Call( CFlushMapForUnattachedDb::ErrGetPersistedFlushMapOrNullObjectIfRuntime( m_wszDatabaseName, m_pdbfilehdr, m_pinst, &m_pfm ) );

HandleError:
    return err;
}

ERR CRBSDatabaseRevertContext::ErrSetDbstateForRevert( ULONG rbsrchkstate, LOGTIME logtimeRevertTo )
{
    Assert( m_pdbfilehdr );
    Assert( m_wszDatabaseName );
    Assert( m_pfapiDb );

    ERR err = JET_errSuccess;

    if ( rbsrchkstate != JET_revertstateNone && m_pdbfilehdr->Dbstate() != JET_dbstateRevertInProgress )
    {
        return ErrERRCheck( JET_errRBSRCBadDbState );
    }

    if ( m_pdbfilehdr->Dbstate() != JET_dbstateRevertInProgress )
    {
        m_pdbfilehdr->SetDbstate( JET_dbstateRevertInProgress, lGenerationInvalid, lGenerationInvalid, NULL, fTrue );
        m_pdbfilehdr->le_ulRevertCount++;
        LGIGetDateTime( &m_pdbfilehdr->logtimeRevertFrom );
        m_pdbfilehdr->le_ulRevertPageCount = 0;
    }

    memcpy( &m_pdbfilehdr->logtimeRevertTo, &logtimeRevertTo, sizeof( LOGTIME ) );

    Call( ErrUtilWriteUnattachedDatabaseHeaders( m_pinst, m_pinst->m_pfsapi, m_wszDatabaseName, m_pdbfilehdr, m_pfapiDb, m_pfm, fFalse ) );
    Call( ErrUtilFlushFileBuffers( m_pfapiDb, iofrRBSRevertUtil ) );

HandleError:
    return err;
}

ERR CRBSDatabaseRevertContext::ErrSetDbstateAfterRevert( ULONG rbsrchkstate )
{
    Assert( rbsrchkstate == JET_revertstateCopingLogs );
    Assert( m_pdbfilehdr );
    Assert( m_pdbfilehdrFromRBS );

    m_pdbfilehdrFromRBS->le_ulRevertCount                           = m_pdbfilehdr->le_ulRevertCount;
    m_pdbfilehdrFromRBS->le_ulRevertPageCount                       = m_pdbfilehdr->le_ulRevertPageCount;
    m_pdbfilehdrFromRBS->le_lgposCommitBeforeRevert                 = lgposMax;
    m_pdbfilehdrFromRBS->le_lgposCommitBeforeRevert.le_lGeneration  = m_pdbfilehdr->le_lGenMaxCommitted;

    memcpy( &m_pdbfilehdrFromRBS->logtimeRevertFrom, &m_pdbfilehdr->logtimeRevertFrom, sizeof( LOGTIME ) );
    memcpy( &m_pdbfilehdrFromRBS->logtimeRevertTo, &m_pdbfilehdr->logtimeRevertTo, sizeof( LOGTIME ) );

    ERR err = JET_errSuccess;

    if ( m_pfm )
    {
        if ( m_pdbfilehdrFromRBS->Dbstate() == JET_dbstateCleanShutdown )
        {
            Call( m_pfm->ErrCleanFlushMap() );
        }
        else
        {
            m_pfm->SetDbGenMinRequired( m_pdbfilehdrFromRBS->le_lGenMinRequired );
            m_pfm->SetDbGenMinConsistent( m_pdbfilehdrFromRBS->le_lGenMinConsistent );

            Call( m_pfm->ErrFlushAllSections( OnDebug( fTrue ) ) );
        }
    }

    Call( ErrUtilWriteUnattachedDatabaseHeaders( m_pinst, m_pinst->m_pfsapi, m_wszDatabaseName, m_pdbfilehdrFromRBS, m_pfapiDb, m_pfm ) );
    Call( ErrUtilFlushFileBuffers( m_pfapiDb, iofrRBSRevertUtil ) );

HandleError:
    return err;
}

ERR CRBSDatabaseRevertContext::ErrRBSCaptureDbHdrFromRBS( RBSDbHdrRecord* prbsdbhdrrec, BOOL* pfGivenDbfilehdrCaptured )
{
    if ( m_pdbfilehdrFromRBS )
    {
        return JET_errSuccess;
    }

    Assert( prbsdbhdrrec->m_usRecLength == sizeof( RBSDbHdrRecord ) + sizeof( DBFILEHDR ) );
    Assert( m_pdbfilehdr );

    ERR err = JET_errSuccess;

    Alloc( m_pdbfilehdrFromRBS = (DBFILEHDR*)PvOSMemoryPageAlloc( g_cbPage, NULL ) );

    DBFILEHDR* pdbfilehdr               = (DBFILEHDR*) prbsdbhdrrec->m_rgbHeader;

    Assert( m_pdbfilehdr->le_dbstate == JET_dbstateRevertInProgress );

    UtilMemCpy( m_pdbfilehdrFromRBS, pdbfilehdr, sizeof( DBFILEHDR ) );

    if ( pfGivenDbfilehdrCaptured )
    {
        *pfGivenDbfilehdrCaptured = fTrue;
    }

HandleError:
    return err;
}

BOOL CRBSDatabaseRevertContext::FPageAlreadyCaptured( PGNO pgno )
{
    BOOL fPageAlreadyCaptured;

    IBitmapAPI::ERR errbm = m_psbmDbPages->ErrGet( pgno, &fPageAlreadyCaptured );    
    Assert( errbm == IBitmapAPI::ERR::errSuccess );

    return fPageAlreadyCaptured;
}

ERR CRBSDatabaseRevertContext::ErrAddPage( void* pvPage, PGNO pgno )
{
    Assert( pvPage );
    Assert( m_rgRBSDbPage );
    Assert( m_rgRBSDbPage->Size() < m_rgRBSDbPage->Capacity() );
    
    IBitmapAPI::ERR             errbm       = IBitmapAPI::ERR::errSuccess;
    CArray< CPagePointer >::ERR errArray    = CArray< CPagePointer >::ERR::errSuccess;
    
    CPagePointer pageptr( (DWORD_PTR) pvPage, pgno );
    errArray = m_rgRBSDbPage->ErrSetEntry( m_rgRBSDbPage->Size(), pageptr );

    if ( CArray< CPagePointer >::ERR::errSuccess != errArray )
    {
        Assert( CArray< CPagePointer >::ERR::errOutOfMemory == errArray );
        return ErrERRCheck( JET_errOutOfMemory );
    }

    errbm = m_psbmDbPages->ErrSet( pgno, fTrue );
    Assert( errbm == IBitmapAPI::ERR::errSuccess );

    return JET_errSuccess;
}

INLINE INT __cdecl CRBSDatabaseRevertContext::ICRBSDatabaseRevertContextCmpPgRec( const CPagePointer* ppg1, const CPagePointer* ppg2 )
{
    Assert( ppg1 );
    Assert( ppg2 );

    Assert( ppg1->PgNo() != ppg2->PgNo() );

    return ( ( ppg1->PgNo() < ppg2->PgNo() ) ? -1 : +1 );
}

void CRBSDatabaseRevertContext::OsWriteIoComplete(
    const ERR errIo,
    IFileAPI* const pfapi,
    const FullTraceContext& tc,
    const OSFILEQOS grbitQOS,
    const QWORD ibOffset,
    const DWORD cbData,
    const BYTE* const pbData,
    const DWORD_PTR keyIOComplete )
{
    CRBSDatabaseRevertContext* const prbsdbrc = (CRBSDatabaseRevertContext*) keyIOComplete;
    prbsdbrc->m_asigWritePossible.Set();
    AtomicDecrement( &(prbsdbrc->m_cpgWritePending) );
}

ERR CRBSDatabaseRevertContext::ErrFlushDBPage( void* pvPage, PGNO pgno, USHORT cbDbPageSize, const OSFILEQOS qos )
{
    Assert( pvPage );
    Assert( pgno > 0 );

    ERR err = JET_errSuccess;
    CPageValidationNullAction nullaction;
    TraceContextScope tcRevertPage( iorpRevertPage );

    QWORD cbSize = 0;
    DBTIME dbtimePage;

    CPAGE cpageT;
    cpageT.LoadPage( ifmpNil, pgno, pvPage, cbDbPageSize );

    if ( m_pfm )
    {
        m_pfm->SetPgnoFlushType( pgno, cpageT.Pgft(), cpageT.Dbtime() );
    }

    dbtimePage = cpageT.Dbtime();
    cpageT.UnloadPage();

    Call( m_pfapiDb->ErrSize( &cbSize, IFileAPI::filesizeLogical ) );

    const QWORD cbNewSize   = OffsetOfPgno( pgno ) + cbDbPageSize;
    if ( cbNewSize > cbSize )
    {
        const QWORD cbNewSizeEffective = roundup( cbNewSize, (QWORD)UlParam( m_pinst, JET_paramDbExtensionSize ) * g_cbPage );
        Call( m_pfapiDb->ErrSetSize( *tcRevertPage, cbNewSizeEffective, fTrue, QosSyncDefault( m_pinst ) ) );
    }

    AtomicIncrement( &m_cpgWritePending ); 

    Call( m_pfapiDb->ErrIOWrite(
                *tcRevertPage,
                OffsetOfPgno( pgno ),
                cbDbPageSize,
                (BYTE*) pvPage,
                qos,
                OsWriteIoComplete,
                (DWORD_PTR)this ) );

    m_pdbfilehdr->le_ulRevertPageCount++;

    (*m_pcprintfRevertTrace)( "Pg %ld,%I64x\r\n", pgno, dbtimePage );

HandleError:
    return err;
}

ERR CRBSDatabaseRevertContext::ErrFlushDBPages( USHORT cbDbPageSize, BOOL fFlushDbHdr, CPG* pcpgReverted )
{
    ERR err = JET_errSuccess;
    m_rgRBSDbPage->Sort( CRBSDatabaseRevertContext::ICRBSDatabaseRevertContextCmpPgRec );
    CPG cpgTotal = m_rgRBSDbPage->Size();
    ULONG ulUrgentLevel = (ULONG) UlParam( m_pinst, JET_paramFlight_RBSRevertIOUrgentLevel );
    OSFILEQOS qosIO = QosOSFileFromUrgentLevel( ulUrgentLevel );

    (*m_pcprintfRevertTrace)( "Flushing database pages for database %ws\r\n", m_wszDatabaseName );

    INT ipgno = 0;
    m_asigWritePossible.Reset();

    while ( ipgno < cpgTotal )
    {
        for ( ; ipgno < cpgTotal; ++ipgno )
        {
            CPagePointer ppTempPage( m_rgRBSDbPage->Entry( ipgno ) );
            Assert( ppTempPage.DwPage() );

            err = ErrFlushDBPage( (void*) ppTempPage.DwPage(), ppTempPage.PgNo(), cbDbPageSize, qosIO );

            if ( err == errDiskTilt )
            {
                AtomicDecrement( &m_cpgWritePending );

                Call( m_pfapiDb->ErrIOIssue() );

                break;
            }
            else
            {
                Call( err );
            }
        }

        m_asigWritePossible.FWait( -1000 );
    }

    Call( m_pfapiDb->ErrIOIssue() );

    while ( m_cpgWritePending != 0 )
    {
        m_asigWritePossible.FWait( -1000 );
    }

    CArray< CPagePointer >::ERR errArray    = CArray< CPagePointer >::ERR::errSuccess;

    for ( int i = 0; i < cpgTotal; ++i )
    {
        CPagePointer ppTempPage( m_rgRBSDbPage->Entry( i ) );

        OSMemoryPageFree( (void*) ppTempPage.DwPage() );
        errArray = m_rgRBSDbPage->ErrSetEntry( i, NULL );

        if ( errArray != CArray< CPagePointer >::ERR::errSuccess )
        {
            Assert( errArray == CArray< CPagePointer >::ERR::errOutOfMemory );
            Error( ErrERRCheck( JET_errOutOfMemory ) );
        }
    }

    errArray = m_rgRBSDbPage->ErrSetSize( 0 );

    if ( errArray != CArray< CPagePointer >::ERR::errSuccess )
    {
        Assert( errArray == CArray< CPagePointer >::ERR::errOutOfMemory );
        Error( ErrERRCheck( JET_errOutOfMemory ) );
    }

    if ( m_pfm )
    {
        m_pfm->SetDbGenMinRequired( max( 1, m_pdbfilehdr->le_lGenMinRequired ) );
        m_pfm->SetDbGenMinConsistent( max( 1, m_pdbfilehdr->le_lGenMinConsistent ) );

        Call( m_pfm->ErrFlushAllSections( OnDebug( fTrue ) ) );
    }

    if ( fFlushDbHdr )
    {
        Call( ErrUtilWriteUnattachedDatabaseHeaders( m_pinst, m_pinst->m_pfsapi, m_wszDatabaseName, m_pdbfilehdr, m_pfapiDb, m_pfm, fFalse ) );
    }

    Call( ErrUtilFlushFileBuffers( m_pfapiDb, iofrRBSRevertUtil ) );

    *pcpgReverted = cpgTotal;

HandleError:
    return err;
}

ERR CRBSDatabaseRevertContext::ErrBeginTracingToIRS()
{
    Assert( m_pinst->m_pfsapi );
    return ErrBeginDatabaseIncReseedTracing( m_pinst->m_pfsapi, m_wszDatabaseName, &m_pcprintfIRSTrace );
}


CRBSRevertContext::CRBSRevertContext( __in INST* const pinst )
    : CZeroInit( sizeof( CRBSRevertContext ) ),
    m_pinst ( pinst )
{
    Assert( pinst );
    m_cpgCacheMax       = (LONG)UlParam( JET_paramCacheSizeMax );
    m_irbsdbrcMaxInUse  = -1;

    for ( DBID dbid = 0; dbid < dbidMax; ++dbid )
    {
        m_mpdbidirbsdbrc[ dbid ] = irbsdbrcInvalid;
    }
}

CRBSRevertContext::~CRBSRevertContext()
{
    if ( m_prbsrchk )
    {
        OSMemoryPageFree( (void*)m_prbsrchk );
        m_prbsrchk = NULL;
    }

    if ( m_pfapirbsrchk )
    {
        delete m_pfapirbsrchk;
        m_pfapirbsrchk = NULL;
    }

    if ( m_wszRBSAbsRootDirPath )
    {
        OSMemoryHeapFree( m_wszRBSAbsRootDirPath );
        m_wszRBSAbsRootDirPath = NULL;
    }

    if ( m_wszRBSBaseName )
    {
        OSMemoryHeapFree( m_wszRBSBaseName );
        m_wszRBSBaseName = NULL;
    }

    for ( IRBSDBRC irbsdbrc = 0; irbsdbrc <= m_irbsdbrcMaxInUse; ++irbsdbrc )
    {
        if ( m_rgprbsdbrcAttached[ irbsdbrc ] )
        {
            delete m_rgprbsdbrcAttached[ irbsdbrc ];
            m_rgprbsdbrcAttached[ irbsdbrc ] = NULL;
        }
    }

    if ( m_pcprintfRevertTrace != NULL &&
        CPRINTFNULL::PcprintfInstance() != m_pcprintfRevertTrace )
    {
        (*m_pcprintfRevertTrace)( "Closing revert tracing file.\r\n" );
        delete m_pcprintfRevertTrace;
    }

    m_pcprintfRevertTrace = NULL;
}

ERR CRBSRevertContext::ErrMakeRevertTracingNames(
    __in IFileSystemAPI* pfsapi,
    __in_range( cbOSFSAPI_MAX_PATHW, cbOSFSAPI_MAX_PATHW ) ULONG cbRBSRCRawPath,
    __out_bcount_z( cbRBSRCRawPath ) WCHAR* wszRBSRCRawPath,
    __in_range( cbOSFSAPI_MAX_PATHW, cbOSFSAPI_MAX_PATHW ) ULONG cbRBSRCRawBackupPath,
    __out_bcount_z( cbRBSRCRawBackupPath ) WCHAR* wszRBSRCRawBackupPath )
{
    Assert( pfsapi );

    ERR err = JET_errSuccess;

    WCHAR * szRevertRawExt = L".RBSRC.RAW";
    WCHAR * szRevertRawBackupExt = L".RBSRC.RAW.Prev";


    wszRBSRCRawPath[0] = L'\0';
    wszRBSRCRawBackupPath[0] = L'\0';

    Call( pfsapi->ErrPathBuild( m_wszRBSAbsRootDirPath, m_wszRBSBaseName, szRevertRawExt, wszRBSRCRawPath ) );
    Call( pfsapi->ErrPathBuild( m_wszRBSAbsRootDirPath, m_wszRBSBaseName, szRevertRawBackupExt, wszRBSRCRawBackupPath ) );

HandleError:

    return err;
}

ERR CRBSRevertContext::ErrBeginRevertTracing( bool fDeleteOldTraceFile )
{
    Assert( m_pinst );
    Assert( m_pinst->m_pfsapi );

    ERR err = JET_errSuccess;

    WCHAR wszRBSRCRawFile[ IFileSystemAPI::cchPathMax ]   = { 0 };
    WCHAR wszRBSRCRawBackupFile[ IFileSystemAPI::cchPathMax ] = { 0 };

    IFileAPI *      pfapiSizeCheck  = NULL;
    IFileSystemAPI* pfsapi          = m_pinst->m_pfsapi;

    m_pcprintfRevertTrace = CPRINTFNULL::PcprintfInstance();

    Call( ErrMakeRevertTracingNames( pfsapi, sizeof(wszRBSRCRawFile), wszRBSRCRawFile, sizeof(wszRBSRCRawBackupFile), wszRBSRCRawBackupFile ) );

    if ( fDeleteOldTraceFile )
    {
        Call( pfsapi->ErrFileDelete( wszRBSRCRawFile ) );
        Call( pfsapi->ErrFileDelete( wszRBSRCRawBackupFile ) );
    }

    err = pfsapi->ErrFileOpen( wszRBSRCRawFile, IFileAPI::fmfNone, &pfapiSizeCheck );
    if ( JET_errSuccess == err )
    {
        QWORD cbSize;
        Call( pfapiSizeCheck->ErrSize( &cbSize, IFileAPI::filesizeLogical ) );
        delete pfapiSizeCheck;
        pfapiSizeCheck = NULL;

        if ( cbSize > ( 50 * 1024 * 1024 )  )
        {
            Call( pfsapi->ErrFileDelete( wszRBSRCRawBackupFile ) );
            Call( pfsapi->ErrFileMove( wszRBSRCRawFile, wszRBSRCRawBackupFile ) );
        }
    }

    CPRINTF * const pcprintfAlloc = new CPRINTFFILE( wszRBSRCRawFile );
    Alloc( pcprintfAlloc );

    m_pcprintfRevertTrace = pcprintfAlloc;

    (*m_pcprintfRevertTrace)( "Please ignore this file.  This is a tracing file for available lag revert information.  You can delete this file if it bothers you.\r\n" );

HandleError:

    if ( pfapiSizeCheck )
    {
        delete pfapiSizeCheck;
        pfapiSizeCheck = NULL;
    }

    return err;
}

BOOL CRBSRevertContext::FRBSDBRC( PCWSTR wszDatabaseName, IRBSDBRC* pirbsdbrc )
{
    for ( IRBSDBRC irbsdbrc = 0; irbsdbrc <= m_irbsdbrcMaxInUse; ++irbsdbrc )
    {
        if ( UtilCmpFileName( wszDatabaseName, m_rgprbsdbrcAttached[ irbsdbrc ]->WszDatabaseName() ) == 0 )
        {
            if ( pirbsdbrc )
            {
                *pirbsdbrc = irbsdbrc;
            }

            return fTrue;
        }
    }

    return fFalse;
}

ERR CRBSRevertContext::ErrRevertCheckpointInit()
{
    Assert( m_wszRBSAbsRootDirPath );
    Assert( m_wszRBSBaseName );

    WCHAR           wszChkFileBase[ IFileSystemAPI::cchPathMax ];
    WCHAR           wszChkFullName[ IFileSystemAPI::cchPathMax ];
    BOOL            fChkFileExists  = fFalse;
    IFileSystemAPI* pfsapi          = m_pinst->m_pfsapi;
    ERR             err             = JET_errSuccess;

    Assert( pfsapi );
    Assert( !m_prbsrchk );
    Assert( !m_pfapirbsrchk );

    Alloc( m_prbsrchk = (RBSREVERTCHECKPOINT *)PvOSMemoryPageAlloc(  sizeof( RBSREVERTCHECKPOINT ), NULL ) );
    memset( m_prbsrchk, 0, sizeof( RBSREVERTCHECKPOINT ) );

    Call( ErrRBSDirPrefix( m_wszRBSBaseName, wszChkFileBase, sizeof( wszChkFileBase ) ) );
    Call( pfsapi->ErrPathBuild( m_wszRBSAbsRootDirPath, wszChkFileBase, wszNewChkExt, wszChkFullName ) );

    fChkFileExists = ( ErrUtilPathExists( pfsapi, wszChkFullName ) == JET_errSuccess );

    if ( fChkFileExists )
    {
        Call( CIOFilePerf::ErrFileOpen( 
            pfsapi, 
            m_pinst, 
            wszChkFullName, 
            BoolParam( JET_paramEnableFileCache ) ? IFileAPI::fmfCached : IFileAPI::fmfNone, 
            iofileRBS, 
            qwRBSRevertChkFileID, 
            &m_pfapirbsrchk ) );

        err = ErrUtilReadShadowedHeader( m_pinst, pfsapi, m_pfapirbsrchk, (BYTE*) m_prbsrchk, sizeof( RBSREVERTCHECKPOINT ), -1, urhfNoAutoDetectPageSize );

        if ( err < JET_errSuccess )
        {
            memset( m_prbsrchk, 0, sizeof( RBSREVERTCHECKPOINT ) );
        }
    }
    else
    {
        Call( CIOFilePerf::ErrFileCreate(
            pfsapi,
            m_pinst,
            wszChkFullName,
            BoolParam( JET_paramEnableFileCache ) ? IFileAPI::fmfCached : IFileAPI::fmfNone,
            iofileRBS,
            qwRBSRevertChkFileID,
            &m_pfapirbsrchk ) );

        LGIGetDateTime( &m_prbsrchk->rbsrchkfilehdr.tmCreate );
        m_prbsrchk->rbsrchkfilehdr.le_filetype = JET_filetypeRBSRevertCheckpoint;

        Call( ErrUtilWriteRBSRevertCheckpointHeaders(
            m_pinst,
            m_pinst->m_pfsapi,
            NULL,
            m_prbsrchk,
            m_pfapirbsrchk ) );
    }

HandleError:
    return err;
}

ERR CRBSRevertContext::ErrRevertCheckpointCleanup()
{
    Assert( m_pfapirbsrchk );
    Assert( m_pinst->m_pfsapi );

    WCHAR           wszChkFullName[ IFileSystemAPI::cchPathMax ];
    m_pfapirbsrchk->ErrPath( wszChkFullName );

    delete m_pfapirbsrchk;
    m_pfapirbsrchk = NULL;

    return m_pinst->m_pfsapi->ErrFileDelete( wszChkFullName );
}

ERR CRBSRevertContext::ErrRBSDBRCInitFromAttachInfo( const BYTE* pbRBSAttachInfo, SIGNATURE* psignRBSHdrFlush )
{
    ERR err                 = JET_errSuccess;

    for ( const BYTE * pbT = pbRBSAttachInfo; 0 != *pbT; pbT += sizeof( RBSATTACHINFO ) )
    {
        RBSATTACHINFO* prbsattachinfo = (RBSATTACHINFO*) pbT;
        CAutoWSZPATH                    wszTempDbAlignedName;
        IRBSDBRC irbsdbrc;

        if ( prbsattachinfo->FPresent() == 0 )
        {
            break;
        }

        CallS( wszTempDbAlignedName.ErrSet( (UnalignedLittleEndian< WCHAR > *)prbsattachinfo->wszDatabaseName ) );

        if ( !FRBSDBRC( wszTempDbAlignedName, &irbsdbrc ) )
        {
            CRBSDatabaseRevertContext* prbsdbrc              = new CRBSDatabaseRevertContext( m_pinst );

            Alloc( prbsdbrc );
            Assert( m_irbsdbrcMaxInUse + 1 < dbidMax );

            m_rgprbsdbrcAttached[ ++m_irbsdbrcMaxInUse ] = prbsdbrc;

            Call( prbsdbrc->ErrRBSDBRCInit( prbsattachinfo, psignRBSHdrFlush, m_cpgCacheMax ) );
            prbsdbrc->SetDbTimePrevDirtied( prbsattachinfo->DbtimePrevDirtied() );
        }
        else if ( m_rgprbsdbrcAttached[ irbsdbrc ]->DbTimePrevDirtied() != prbsattachinfo->DbtimeDirtied() )
        {
            Error( ErrERRCheck( JET_errRBSRCInvalidRBS ) );
        }
        else
        {
            m_rgprbsdbrcAttached[ irbsdbrc ]->SetDbTimePrevDirtied( prbsattachinfo->DbtimePrevDirtied() );
        }
    }

HandleError:
    return err;
}

ERR CRBSRevertContext::ErrComputeRBSRangeToApply( LOGTIME ltRevertExpected, LOGTIME* pltRevertActual )
{
    Assert( m_prbsrchk );

    LONG        lRBSGenMin, lRBSGenMax;
    WCHAR       wszRBSAbsDirPath[ IFileSystemAPI::cchPathMax ];
    WCHAR       wszRBSAbsFilePath[ IFileSystemAPI::cchPathMax ];
    LOGTIME     tmPrevRBSGen;
    IFileAPI*   pfileapi            = NULL;
    ERR         err                 = JET_errSuccess;    

    Call( ErrRBSGetLowestAndHighestGen_( m_pinst->m_pfsapi, m_wszRBSAbsRootDirPath, m_wszRBSBaseName, &lRBSGenMin, &lRBSGenMax ) );

    for ( LONG rbsGen = lRBSGenMax; rbsGen >= lRBSGenMin && rbsGen > 0; rbsGen-- )
    {
        RBSFILEHDR rbsfilehdr;

        Call( ErrRBSFilePathForGen_( m_wszRBSAbsRootDirPath, m_wszRBSBaseName, m_pinst->m_pfsapi, wszRBSAbsDirPath, sizeof( wszRBSAbsDirPath ), wszRBSAbsFilePath, cbOSFSAPI_MAX_PATHW, rbsGen ) );
        Call( ErrRBSLoadRbsGen( m_pinst, wszRBSAbsFilePath, rbsGen, &rbsfilehdr, &pfileapi ) );
        Call( ErrRBSDBRCInitFromAttachInfo( rbsfilehdr.rgbAttach, &rbsfilehdr.rbsfilehdr.signRBSHdrFlush ) );

        if ( LOGTIME::CmpLogTime( rbsfilehdr.rbsfilehdr.tmCreate, ltRevertExpected ) > 0 )
        {
            if ( !rbsfilehdr.rbsfilehdr.tmPrevGen.FIsSet() )
            {
                Error( ErrERRCheck( JET_errRBSRCInvalidRBS ) );
            }

            if ( rbsGen != lRBSGenMax )
            {
                if ( LOGTIME::CmpLogTime( rbsfilehdr.rbsfilehdr.tmCreate, tmPrevRBSGen ) != 0 )
                {
                    Error( ErrERRCheck( JET_errRBSRCInvalidRBS ) );
                }
            }
            
            tmPrevRBSGen = rbsfilehdr.rbsfilehdr.tmPrevGen;
        }
        else
        {
            m_lRBSMinGenToApply = rbsfilehdr.rbsfilehdr.le_lGeneration;
            m_lRBSMaxGenToApply = lRBSGenMax;
            UtilMemCpy( pltRevertActual, &rbsfilehdr.rbsfilehdr.tmCreate, sizeof( LOGTIME ) );
            break;
        }

        if ( pfileapi != NULL )
        {
            delete pfileapi;
            pfileapi = NULL;
        }
    }

    if ( m_lRBSMaxGenToApply == 0 )
    {
        Error( ErrERRCheck( JET_errRBSRCNoRBSFound ) );
    }

    Assert( m_lRBSMinGenToApply > 0 );

HandleError:
    if ( err == JET_errReadVerifyFailure )
    {
        ErrERRCheck( JET_errRBSRCInvalidRBS );
    }

    if ( pfileapi != NULL )
    {
        delete pfileapi;
    }

    return err;
}

ERR CRBSRevertContext::ErrUpdateRevertTimeFromCheckpoint( LOGTIME* pltRevertExpected )
{
    Assert( m_prbsrchk );

    if ( m_prbsrchk->rbsrchkfilehdr.le_rbsrevertstate != JET_revertstateNone )
    {
        INT fCompare = LOGTIME::CmpLogTime( m_prbsrchk->rbsrchkfilehdr.tmCreateCurrentRBSGen, *pltRevertExpected );
        
        if ( fCompare < 0 )
        {
            UtilMemCpy( pltRevertExpected, &m_prbsrchk->rbsrchkfilehdr.tmCreateCurrentRBSGen, sizeof( LOGTIME ) );
        }
        else if ( m_prbsrchk->rbsrchkfilehdr.le_rbsrevertstate == JET_revertstateCopingLogs && fCompare > 0 )
        {
            return ErrERRCheck( JET_errRBSRCCopyLogsRevertState );
        }
    }

    return JET_errSuccess;
}

VOID CRBSRevertContext::UpdateRBSGenToApplyFromCheckpoint()
{
    Assert( m_prbsrchk );

    if ( m_prbsrchk->rbsrchkfilehdr.le_rbsrevertstate != JET_revertstateNone )
    {
        m_lRBSMaxGenToApply = m_prbsrchk->rbsrchkfilehdr.le_rbsposCheckpoint.le_lGeneration;
        Assert( m_lRBSMaxGenToApply >= m_lRBSMinGenToApply );
    }
}

ERR CRBSRevertContext::ErrUpdateRevertCheckpoint( ULONG revertstate, RBS_POS rbspos, LOGTIME tmCreateCurrentRBSGen, BOOL fUpdateRevertedPageCount )
{
    Assert( revertstate != JET_revertstateNone );
    Assert( m_pfapirbsrchk );
    Assert( m_prbsrchk );

    __int64 ftSinceLastUpdate = UtilGetCurrentFileTime() - m_ftRevertLastUpdate;
    LONG lGenMinReq = lGenerationMax;
    LONG lGenMaxReq = 0;

    switch ( revertstate )
    {
        case JET_revertstateInProgress:
        {
            AssertRTL( m_prbsrchk->rbsrchkfilehdr.le_rbsrevertstate == JET_revertstateNone ||
                m_prbsrchk->rbsrchkfilehdr.le_rbsrevertstate == JET_revertstateInProgress );
            break;
        }
        case JET_revertstateCopingLogs:
        {
            AssertRTL( m_prbsrchk->rbsrchkfilehdr.le_rbsrevertstate == JET_revertstateInProgress );
            break;
        }        
    }

    if ( m_ftRevertLastUpdate != 0 )
    {
        m_prbsrchk->rbsrchkfilehdr.le_cSecInRevert = m_prbsrchk->rbsrchkfilehdr.le_cSecInRevert + UtilConvertFileTimeToSeconds( ftSinceLastUpdate );

        m_ftRevertLastUpdate = UtilGetCurrentFileTime();
    }

    if ( m_irbsdbrcMaxInUse > 0 )
    {
        if ( m_prbsrchk->rbsrchkfilehdr.le_lGenMinRevertStart == 0 || m_prbsrchk->rbsrchkfilehdr.le_lGenMaxRevertStart == 0 )
        {            
            for ( IRBSDBRC irbsdbrc = 0; irbsdbrc <= m_irbsdbrcMaxInUse; ++irbsdbrc )
            {
                LONG lgenMinReqDb = m_rgprbsdbrcAttached[ irbsdbrc ]->LGenMinRequired();
                LONG lgenMaxReqDb = m_rgprbsdbrcAttached[ irbsdbrc ]->LGenMaxRequired();

                Assert( lgenMinReqDb <= lgenMaxReqDb );
                Assert( ( lgenMinReqDb != 0 && lgenMaxReqDb !=0 ) || lgenMinReqDb == lgenMaxReqDb );
                
                if ( lgenMinReqDb == 0 )
                {
                    lgenMinReqDb = m_rgprbsdbrcAttached[ irbsdbrc ]->LgenLastConsistent();
                    lgenMaxReqDb = m_rgprbsdbrcAttached[ irbsdbrc ]->LgenLastConsistent();
                }

                lGenMinReq = min( lGenMinReq, lgenMinReqDb );
                lGenMaxReq = max( lGenMaxReq, lgenMaxReqDb );
            }

            m_prbsrchk->rbsrchkfilehdr.le_lGenMinRevertStart = lGenMinReq;
            m_prbsrchk->rbsrchkfilehdr.le_lGenMaxRevertStart = lGenMaxReq;
        }
    }

    if ( !m_prbsrchk->rbsrchkfilehdr.tmCreate.FIsSet() )
    {
        LGIGetDateTime( &m_prbsrchk->rbsrchkfilehdr.tmCreate );
        m_prbsrchk->rbsrchkfilehdr.le_filetype = JET_filetypeRBSRevertCheckpoint;
    }

    if ( !m_prbsrchk->rbsrchkfilehdr.tmExecuteRevertBegin.FIsSet() && revertstate == JET_revertstateInProgress )
    {
        LGIGetDateTime( &m_prbsrchk->rbsrchkfilehdr.tmExecuteRevertBegin );
    }

    if ( fUpdateRevertedPageCount )
    {
        LittleEndian<QWORD> le_cpgRevertedCurRBSGen = m_cPagesRevertedCurRBSGen;
        m_prbsrchk->rbsrchkfilehdr.le_cPagesReverted += le_cpgRevertedCurRBSGen;
        m_cPagesRevertedCurRBSGen = 0;
    }

    Assert( m_prbsrchk->rbsrchkfilehdr.le_rbsposCheckpoint.le_lGeneration >= rbspos.lGeneration ||  
            ( m_prbsrchk->rbsrchkfilehdr.le_rbsposCheckpoint.le_lGeneration == 0 && 
              m_prbsrchk->rbsrchkfilehdr.le_rbsrevertstate == JET_revertstateNone ) );
    Assert( !m_prbsrchk->rbsrchkfilehdr.tmCreateCurrentRBSGen.FIsSet() ||
            LOGTIME::CmpLogTime( tmCreateCurrentRBSGen, m_prbsrchk->rbsrchkfilehdr.tmCreateCurrentRBSGen ) <= 0 );

    m_prbsrchk->rbsrchkfilehdr.le_rbsposCheckpoint = rbspos;
    m_prbsrchk->rbsrchkfilehdr.tmCreateCurrentRBSGen = tmCreateCurrentRBSGen;

    m_prbsrchk->rbsrchkfilehdr.le_rbsrevertstate = revertstate;

    return ErrUtilWriteRBSRevertCheckpointHeaders(
                m_pinst,
                m_pinst->m_pfsapi,
                NULL,
                m_prbsrchk,
                m_pfapirbsrchk );
}

ERR CRBSRevertContext::ErrInitContext( LOGTIME ltRevertExpected, LOGTIME* pltRevertActual, CPG cpgCache, BOOL fDeleteExistingLogs )
{
    ERR err = JET_errSuccess;
    WCHAR wszRBSAbsLogPath[ IFileSystemAPI::cchPathMax ];
    RBSFILEHDR rbsfilehdr;

    Call( ErrRBSInitPaths_( m_pinst, &m_wszRBSAbsRootDirPath, &m_wszRBSBaseName ) );

    Assert( m_wszRBSBaseName );
    Assert( m_wszRBSBaseName[ 0 ] );

    Assert( m_wszRBSAbsRootDirPath );
    Assert( m_wszRBSAbsRootDirPath[ 0 ] );

    m_cpgCacheMax = cpgCache;
    Call( ErrRevertCheckpointInit() );

    Call ( ErrUpdateRevertTimeFromCheckpoint( &ltRevertExpected ) );

    Call( ErrComputeRBSRangeToApply( ltRevertExpected, pltRevertActual ) );
    UpdateRBSGenToApplyFromCheckpoint();

    m_ltRevertTo            = *pltRevertActual;
    m_fDeleteExistingLogs   = fDeleteExistingLogs;

    Call( ErrRBSPerformLogChecks( m_pinst, m_wszRBSAbsRootDirPath, m_wszRBSBaseName, m_lRBSMinGenToApply, !fDeleteExistingLogs, &rbsfilehdr, wszRBSAbsLogPath ) );

HandleError:
    return err;
}

ERR CRBSRevertContext::ErrRBSRevertContextInit( 
                INST*       pinst, 
        __in    LOGTIME     ltRevertExpected,
        __in    CPG         cpgCache,
        __in    JET_GRBIT   grbit,
        _Out_   LOGTIME*    pltRevertActual,
        _Out_   CRBSRevertContext**    pprbsrc )
{
    Assert( pprbsrc );
    Assert( *pprbsrc == NULL );

    ERR      err     = JET_errSuccess;
    CRBSRevertContext*  prbsrc  = NULL;

    Alloc( prbsrc = new CRBSRevertContext( pinst ) );
    Call( prbsrc->ErrInitContext( ltRevertExpected, pltRevertActual, cpgCache, grbit & JET_bitDeleteAllExistingLogs ) );   
    *pprbsrc = prbsrc;
    return JET_errSuccess;

HandleError:
    if ( prbsrc != NULL )
    {
        delete prbsrc;
    }

    return err;
}

ERR CRBSRevertContext::ErrAddPageRecord( void* pvPage, DBID dbid, PGNO pgno )
{
    Assert( m_mpdbidirbsdbrc[ dbid ] != irbsdbrcInvalid );
    Assert( m_mpdbidirbsdbrc[ dbid ] <= m_irbsdbrcMaxInUse );
    Assert( m_rgprbsdbrcAttached[ m_mpdbidirbsdbrc[ dbid ] ] );
    
    ERR err = JET_errSuccess;

    Call( m_rgprbsdbrcAttached[ m_mpdbidirbsdbrc[ dbid ] ]->ErrAddPage( pvPage, pgno ) );
    m_cpgCached++;

HandleError:
    return err;
}

BOOL CRBSRevertContext::FPageAlreadyCaptured( DBID dbid, PGNO pgno )
{
    Assert( m_mpdbidirbsdbrc[ dbid ] != irbsdbrcInvalid );
    Assert( m_mpdbidirbsdbrc[ dbid ] <= m_irbsdbrcMaxInUse );
    Assert( m_rgprbsdbrcAttached[ m_mpdbidirbsdbrc[ dbid ] ] );

    return m_rgprbsdbrcAttached[ m_mpdbidirbsdbrc[ dbid ] ]->FPageAlreadyCaptured( pgno );
}

ERR CRBSRevertContext::ErrApplyRBSRecord( RBSRecord* prbsrec, BOOL fCaptureDbHdrFromRBS, BOOL fDbHeaderOnly, BOOL* pfGivenDbfilehdrCaptured )
{
    BYTE                bRecType        = prbsrec->m_bRecType;
    void*               pvPage          = NULL;
    ERR                 err             = JET_errSuccess;

    if ( pfGivenDbfilehdrCaptured )
    {
        *pfGivenDbfilehdrCaptured = fFalse;
    }

    switch ( bRecType )
    {
        case rbsrectypeDbHdr:
        {
            if ( !fCaptureDbHdrFromRBS )
            {
                break;
            }

            RBSDbHdrRecord* prbsdbhdrrec    = ( RBSDbHdrRecord* ) prbsrec;
            Assert( prbsdbhdrrec->m_dbid < dbidMax );

            IRBSDBRC irbsdbrc               = m_mpdbidirbsdbrc[ prbsdbhdrrec->m_dbid ];
            CRBSDatabaseRevertContext* prbsdbrc              = NULL;

            Assert( irbsdbrc != irbsdbrcInvalid );
            Assert( irbsdbrc <= m_irbsdbrcMaxInUse );

            prbsdbrc = m_rgprbsdbrcAttached[ irbsdbrc ];
            Assert( prbsdbrc );
            Assert( prbsdbrc->DBIDCurrent() < dbidMax );

            prbsdbrc->ErrRBSCaptureDbHdrFromRBS( prbsdbhdrrec, pfGivenDbfilehdrCaptured );

            break;
        }

        case rbsrectypeDbAttach:
        {
            IRBSDBRC irbsdbrc                   = irbsdbrcInvalid;
            BOOL     fRBSDBRC                   = fFalse;
            CRBSDatabaseRevertContext* prbsdbrc = NULL;
            RBSDbAttachRecord* prbsdbatchrec    = ( RBSDbAttachRecord* ) prbsrec;
            CAutoWSZPATH wszDbName;
            Call( wszDbName.ErrSet( prbsdbatchrec->m_wszDbName ) );

            fRBSDBRC = FRBSDBRC( wszDbName, &irbsdbrc );

            Assert( fRBSDBRC );
            Assert( irbsdbrc != irbsdbrcInvalid );
            Assert( irbsdbrc <= m_irbsdbrcMaxInUse );

            prbsdbrc = m_rgprbsdbrcAttached[ irbsdbrc ];
            Assert( prbsdbrc );
            Assert( prbsdbatchrec->m_dbid < dbidMax );

            if ( prbsdbatchrec->m_dbid != prbsdbrc->DBIDCurrent() )
            {
                if ( prbsdbrc->DBIDCurrent() == dbidMax )
                {
                    prbsdbrc->SetDBIDCurrent( prbsdbatchrec->m_dbid );
                }
                else if ( m_mpdbidirbsdbrc[ prbsdbrc->DBIDCurrent() ] == irbsdbrc )
                {
                    m_mpdbidirbsdbrc[ prbsdbrc->DBIDCurrent() ] = irbsdbrcInvalid;
                }

                if ( m_mpdbidirbsdbrc[ prbsdbatchrec->m_dbid ] != irbsdbrcInvalid )
                {
                    m_rgprbsdbrcAttached[ m_mpdbidirbsdbrc[ prbsdbatchrec->m_dbid ] ]->SetDBIDCurrent( dbidMax ); 
                }

                m_mpdbidirbsdbrc[ prbsdbatchrec->m_dbid ] = irbsdbrc;
            }
            else if ( m_mpdbidirbsdbrc[ prbsdbrc->DBIDCurrent() ] != irbsdbrc )
            {
                Assert( m_mpdbidirbsdbrc[ prbsdbrc->DBIDCurrent() ] == irbsdbrcInvalid );                
                m_mpdbidirbsdbrc[ prbsdbatchrec->m_dbid ] = irbsdbrc;
            }
            
            break;
        }
        case rbsrectypeDbPage:
        {
            if ( fDbHeaderOnly )
            {
                return JET_errSuccess;
            }

            DATA dataImage;

            RBSDbPageRecord* prbsdbpgrec = ( RBSDbPageRecord* ) prbsrec;
            dataImage.SetPv( prbsdbpgrec->m_rgbData );
            dataImage.SetCb( prbsdbpgrec->m_usRecLength - sizeof(RBSDbPageRecord) );
            
            if ( !FPageAlreadyCaptured( prbsdbpgrec->m_dbid, prbsdbpgrec->m_pgno ) )
            {
                pvPage = PvOSMemoryPageAlloc( m_cbDbPageSize, NULL );
                Alloc( pvPage );

                if ( prbsdbpgrec->m_fFlags )
                {
                    Call( ErrRBSDecompressPreimage( dataImage, m_cbDbPageSize, (BYTE*) pvPage, prbsdbpgrec->m_pgno, prbsdbpgrec->m_fFlags ) );
                }
                else
                {
                    UtilMemCpy( pvPage, prbsdbpgrec->m_rgbData, dataImage.Cb() );
                }

                Assert( m_cbDbPageSize == dataImage.Cb() );

                CPAGE cpage;
                cpage.LoadPage( ifmpNil, prbsdbpgrec->m_pgno, pvPage, m_cbDbPageSize );
                cpage.PreparePageForWrite( CPAGE::PageFlushType::pgftUnknown, fTrue, fTrue );
                cpage.UnloadPage();

                Call( ErrAddPageRecord( pvPage, prbsdbpgrec->m_dbid, prbsdbpgrec->m_pgno ) );
            }

            break;
        }
        
        case rbsrectypeDbNewPage:
        {
            if ( fDbHeaderOnly )
            {
                return JET_errSuccess;
            }

            RBSDbNewPageRecord* prbsdbnewpgrec = ( RBSDbNewPageRecord* ) prbsrec;

            if ( !FPageAlreadyCaptured( prbsdbnewpgrec->m_dbid, prbsdbnewpgrec->m_pgno ) )
            {
                pvPage = PvOSMemoryPageAlloc( m_cbDbPageSize, NULL );
                Alloc( pvPage );

                CPAGE cpage;
                cpage.GetRevertedNewPage( prbsdbnewpgrec->m_pgno, pvPage, m_cbDbPageSize );
                Assert( cpage.Pgft() == CPAGE::PageFlushType::pgftUnknown );

                Call( ErrAddPageRecord( pvPage, prbsdbnewpgrec->m_dbid, prbsdbnewpgrec->m_pgno ) );
            }

            break;
        }

        default:
            Call( ErrERRCheck( JET_errRBSInvalidRecord ) );
    }

    return JET_errSuccess;

HandleError:
    if ( pvPage )
    {
        OSMemoryPageFree( pvPage );
    }

    return err;
}

ERR CRBSRevertContext::ErrFlushPages( BOOL fFlushDbHdr )
{
    ERR err = JET_errSuccess;
    
    for ( IRBSDBRC irbsdbrc = 0; irbsdbrc <= m_irbsdbrcMaxInUse; ++irbsdbrc )
    {
        CPG cpgReverted;
        Call( m_rgprbsdbrcAttached[ irbsdbrc ]->ErrFlushDBPages( m_cbDbPageSize, fFlushDbHdr, &cpgReverted ) );
        m_cPagesRevertedCurRBSGen += cpgReverted;
    }

    m_cpgCached = 0;

HandleError:
    return err;
}

ERR CRBSRevertContext::ErrRBSGenApply( LONG lRBSGen, BOOL fDbHeaderOnly )
{
    ERR              err = JET_errSuccess;

    WCHAR wszRBSAbsDirPath[ IFileSystemAPI::cchPathMax ];
    WCHAR wszRBSAbsFilePath[ IFileSystemAPI::cchPathMax ];
    WCHAR wszErrorReason[ cbOSFSAPI_MAX_PATHW ];
    
    RBS_POS             rbsposRecStart          = rbsposMin;
    CRevertSnapshot*    prbs                    = NULL;
    IFileAPI*           pfapirbs                = NULL;
    RBSRecord*          prbsRecord              = NULL;
    RBS_POS             rbspos                  = { 0, lRBSGen };
    BOOL                fGivenDbfilehdrCaptured = fFalse;

    Assert( lRBSGen == m_lRBSMinGenToApply || !fDbHeaderOnly );
    Assert( m_irbsdbrcMaxInUse >= 0 );

    (*m_pcprintfRevertTrace)( "RBSGen - %ld, DbHeaderOnly - %ld.\r\n", lRBSGen, fDbHeaderOnly );
    Call( ErrRBSFilePathForGen_( m_wszRBSAbsRootDirPath, m_wszRBSBaseName, m_pinst->m_pfsapi, wszRBSAbsDirPath, sizeof( wszRBSAbsDirPath ), wszRBSAbsFilePath, cbOSFSAPI_MAX_PATHW, lRBSGen ) );
    Call( CIOFilePerf::ErrFileOpen( m_pinst->m_pfsapi, m_pinst, wszRBSAbsFilePath, IFileAPI::fmfReadOnly, iofileRBS, qwRBSFileID, &pfapirbs ) );

    Alloc( prbs = new CRevertSnapshot( m_pinst ) );
    Call( prbs->ErrSetRBSFileApi( pfapirbs ) );

    Assert( prbs->RBSFileHdr() );    

    if ( !fDbHeaderOnly )
    {
        Call( ErrUpdateRevertCheckpoint( JET_revertstateInProgress, rbspos, prbs->RBSFileHdr()->rbsfilehdr.tmCreate, fTrue ) );
    }

    if ( lRBSGen < m_lRBSMaxGenToApply )
    {
        Call( ErrFaultInjection( 50123 ) );
    }

    m_cbDbPageSize = prbs->RBSFileHdr()->rbsfilehdr.le_cbDbPageSize;

    err = prbs->ErrGetNextRecord( &prbsRecord, &rbsposRecStart, wszErrorReason );

    while ( err != JET_wrnNoMoreRecords && err == JET_errSuccess )
    {
        Call( ErrApplyRBSRecord( prbsRecord, lRBSGen == m_lRBSMinGenToApply, fDbHeaderOnly, &fGivenDbfilehdrCaptured ) );

        if ( m_cpgCached >= m_cpgCacheMax )
        {
            Call( ErrFlushPages( fFalse ) );
        }

        if ( m_fRevertCancelled )
        {
            Error( ErrERRCheck( JET_errRBSRCRevertCancelled ) );
        }

        if ( fDbHeaderOnly && fGivenDbfilehdrCaptured )
        {
            BOOL fAllCaptured = fTrue;

            for ( IRBSDBRC irbsdbrc = 0; irbsdbrc <= m_irbsdbrcMaxInUse; ++irbsdbrc )
            {
                fAllCaptured = fAllCaptured && m_rgprbsdbrcAttached[ irbsdbrc ]->PDbfilehdrFromRBS();
            }

            if ( fAllCaptured )
            {
                break;
            }
        }

        err = prbs->ErrGetNextRecord( &prbsRecord, &rbsposRecStart, wszErrorReason );
    }

    if ( err == JET_wrnNoMoreRecords && !fDbHeaderOnly )
    {
        Call( ErrFlushPages( fTrue ) );
        err = JET_errSuccess;
    }
    
HandleError:
    if ( prbs )
    {
        delete prbs;
        prbs = NULL;
    }

    if ( m_pcprintfRevertTrace )
    {
        TraceFuncComplete( m_pcprintfRevertTrace, __FUNCTION__, err );
    }

    return err;
}

ERR CRBSRevertContext::ErrUpdateDbStatesAfterRevert()
{
    WCHAR wszPathJetChkLog[ IFileSystemAPI::cchPathMax ];
    ERR err = JET_errSuccess;

    Assert( m_pinst->m_plog );

    CallS( m_pinst->m_pfsapi->ErrPathBuild( 
        SzParam( m_pinst, JET_paramSystemPath ), 
        SzParam( m_pinst, JET_paramBaseName ), 
        ( UlParam( m_pinst, JET_paramLegacyFileNames ) & JET_bitESE98FileNames ) ? wszOldChkExt : wszNewChkExt, 
        wszPathJetChkLog ) );

    Assert( m_pinst->m_pfsapi );

    err = m_pinst->m_pfsapi->ErrFileDelete( wszPathJetChkLog );

    Call( err == JET_errFileNotFound ? JET_errSuccess : err );

    Assert( m_prbsrchk );

    for ( IRBSDBRC irbsdbrc = 0; irbsdbrc <= m_irbsdbrcMaxInUse; ++irbsdbrc )
    {
        m_rgprbsdbrcAttached[ irbsdbrc ]->ErrSetDbstateAfterRevert( m_prbsrchk->rbsrchkfilehdr.le_rbsrevertstate );
    }

HandleError:
    return err;
}

ERR CRBSRevertContext::ErrManageStateAfterRevert( LONG* pLgenNewMinReq, LONG* pLgenNewMaxReq )
{
    WCHAR wszRBSAbsLogPath[ IFileSystemAPI::cchPathMax ];
    RBSFILEHDR rbsfilehdr;

    ERR     err         = JET_errSuccess;

    Call( ErrRBSPerformLogChecks( m_pinst, m_wszRBSAbsRootDirPath, m_wszRBSBaseName, m_lRBSMinGenToApply, !m_fDeleteExistingLogs, &rbsfilehdr, wszRBSAbsLogPath ) );
 
    if ( m_fDeleteExistingLogs )
    {    
        WCHAR wszLogDeleteFilter[ IFileSystemAPI::cchPathMax ];
        Call( ErrOSStrCbCopyW( wszLogDeleteFilter, sizeof( wszLogDeleteFilter ), L"*" ) );
        Call( ErrOSStrCbAppendW( wszLogDeleteFilter, sizeof( wszLogDeleteFilter ), ( UlParam( m_pinst, JET_paramLegacyFileNames ) & JET_bitESE98FileNames ) ? wszOldLogExt : wszNewLogExt ) );
        Call( ErrRBSDeleteAllFiles( m_pinst->m_pfsapi, SzParam( m_pinst, JET_paramLogFilePath ), wszLogDeleteFilter, fFalse ) );
    }

    if ( m_prbsrchk->rbsrchkfilehdr.le_rbsrevertstate != JET_revertstateCopingLogs )
    {
        Call( ErrUpdateRevertCheckpoint( JET_revertstateCopingLogs, m_prbsrchk->rbsrchkfilehdr.le_rbsposCheckpoint, m_prbsrchk->rbsrchkfilehdr.tmCreateCurrentRBSGen, fFalse ) );
    }

    Call( ErrRBSCopyRequiredLogs_(
        m_pinst,
        rbsfilehdr.rbsfilehdr.le_lGenMinLogCopied,
        rbsfilehdr.rbsfilehdr.le_lGenMaxLogCopied,
        wszRBSAbsLogPath,
        SzParam( m_pinst, JET_paramLogFilePath ),
        fFalse,
        m_fDeleteExistingLogs ) );

    Call( ErrFaultInjection( 50124 ) );

    Call( ErrUpdateDbStatesAfterRevert() );

    if ( pLgenNewMinReq )
    {
        *pLgenNewMinReq = rbsfilehdr.rbsfilehdr.le_lGenMinLogCopied;
    }

    if ( pLgenNewMaxReq )
    {
        *pLgenNewMaxReq = rbsfilehdr.rbsfilehdr.le_lGenMaxLogCopied;
    }

HandleError:
    if ( err < JET_errSuccess && m_pcprintfRevertTrace )
    {
        TraceFuncComplete( m_pcprintfRevertTrace, __FUNCTION__, err );
    }

    return err;
}

ERR CRBSRevertContext::ErrExecuteRevert( JET_GRBIT grbit, JET_RBSREVERTINFOMISC*  prbsrevertinfo )
{
    Assert( m_lRBSMaxGenToApply > 0 );
    Assert( m_lRBSMinGenToApply > 0 );
    Assert( m_lRBSMinGenToApply <= m_lRBSMaxGenToApply );

    LONG lGenNewMinRequired;
    LONG lGenNewMaxRequired;    
    size_t  cchRequired;
    LittleEndian<QWORD> le_cPagesRevertedCurRBSGen;
    __int64 ftSinceLastUpdate   = 0;

    m_fRevertCancelled      = fFalse;
    m_fExecuteRevertStarted = fTrue;
    m_ftRevertLastUpdate    = UtilGetCurrentFileTime();

    ERR err = JET_errSuccess;

    Call( ErrBeginRevertTracing( m_prbsrchk->rbsrchkfilehdr.le_rbsrevertstate == JET_revertstateNone || m_prbsrchk->rbsrchkfilehdr.le_rbsrevertstate == JET_revertstateCompleted ) );

    TraceFuncBegun( m_pcprintfRevertTrace, __FUNCTION__ );

    for ( IRBSDBRC irbsdbrc = 0; irbsdbrc <= m_irbsdbrcMaxInUse; ++irbsdbrc )
    {
        m_rgprbsdbrcAttached[ irbsdbrc ]->SetPrintfTrace( m_pcprintfRevertTrace );
        Call( m_rgprbsdbrcAttached[ irbsdbrc ]->ErrBeginTracingToIRS() );
        TraceFuncBegun( m_rgprbsdbrcAttached[ irbsdbrc ]->CprintfIRSTrace(), __FUNCTION__ );
    }

    if ( m_prbsrchk->rbsrchkfilehdr.le_rbsrevertstate != JET_revertstateCopingLogs )
    {
        for ( IRBSDBRC irbsdbrc = 0; irbsdbrc <= m_irbsdbrcMaxInUse; ++irbsdbrc )
        {
            Call( m_rgprbsdbrcAttached[ irbsdbrc ]->ErrSetDbstateForRevert( m_prbsrchk->rbsrchkfilehdr.le_rbsrevertstate, m_ltRevertTo ) );
        }

        for ( LONG lRBSGen = m_lRBSMaxGenToApply; lRBSGen >= m_lRBSMinGenToApply; --lRBSGen )
        {
            Call( ErrRBSGenApply( lRBSGen, fFalse ) );

            for ( IRBSDBRC irbsdbrc = 0; irbsdbrc <= m_irbsdbrcMaxInUse; ++irbsdbrc )
            {
                Call( m_rgprbsdbrcAttached[ irbsdbrc ]->ErrResetSbmDbPages() );
            }

            if ( m_fRevertCancelled )
            {
                Error( ErrERRCheck( JET_errRBSRCRevertCancelled ) );
            }
        }
    }
    else
    {
        Call( ErrRBSGenApply( m_lRBSMinGenToApply, fTrue ) );
    }
    
    Call( ErrManageStateAfterRevert( &lGenNewMinRequired, &lGenNewMaxRequired ) );

    ftSinceLastUpdate = UtilGetCurrentFileTime() - m_ftRevertLastUpdate;

    le_cPagesRevertedCurRBSGen  = m_cPagesRevertedCurRBSGen;

    memcpy( &prbsrevertinfo->logtimeRevertFrom, &m_prbsrchk->rbsrchkfilehdr.tmExecuteRevertBegin, sizeof( LOGTIME ) );
    prbsrevertinfo->cSecRevert          = m_prbsrchk->rbsrchkfilehdr.le_cSecInRevert + UtilConvertFileTimeToSeconds( ftSinceLastUpdate );
    prbsrevertinfo->cPagesReverted      = m_prbsrchk->rbsrchkfilehdr.le_cPagesReverted + le_cPagesRevertedCurRBSGen;
    prbsrevertinfo->lGenMinRevertStart  = m_prbsrchk->rbsrchkfilehdr.le_lGenMinRevertStart;
    prbsrevertinfo->lGenMaxRevertStart  = m_prbsrchk->rbsrchkfilehdr.le_lGenMaxRevertStart;
    prbsrevertinfo->lGenMinRevertEnd    = lGenNewMinRequired;
    prbsrevertinfo->lGenMaxRevertEnd    = lGenNewMaxRequired;

    TraceFuncComplete( m_pcprintfRevertTrace, __FUNCTION__, JET_errSuccess );

    for ( IRBSDBRC irbsdbrc = 0; irbsdbrc <= m_irbsdbrcMaxInUse; ++irbsdbrc )
    {
        TraceFuncComplete( m_rgprbsdbrcAttached[ irbsdbrc ]->CprintfIRSTrace(), __FUNCTION__, JET_errSuccess );
    }      

    Call( ErrRevertCheckpointCleanup() );    

HandleError:
    __int64 fileTimeRevertFrom  = ConvertLogTimeToFileTime( &m_prbsrchk->rbsrchkfilehdr.tmExecuteRevertBegin );
    __int64 fileTimeRevertTo    = ConvertLogTimeToFileTime( &m_ltRevertTo );

    WCHAR   wszTimeFrom[32];
    WCHAR   wszDateFrom[32];
    
    WCHAR   wszTimeTo[32];
    WCHAR   wszDateTo[32];

    ErrUtilFormatFileTimeAsTimeWithSeconds( fileTimeRevertFrom, wszTimeFrom, _countof(wszTimeFrom), &cchRequired );
    ErrUtilFormatFileTimeAsDate( fileTimeRevertFrom, wszDateFrom, _countof( wszDateFrom ), &cchRequired );

    ErrUtilFormatFileTimeAsTimeWithSeconds( fileTimeRevertTo, wszTimeTo, _countof(wszTimeTo), &cchRequired );
    ErrUtilFormatFileTimeAsDate( fileTimeRevertTo, wszDateTo, _countof( wszDateTo ), &cchRequired );

    const WCHAR* rgcwsz[] =
    {
        OSFormatW( L"%ws %ws", wszTimeFrom, wszDateFrom ),
        OSFormatW( L"%ws %ws", wszTimeTo, wszDateTo ),
        OSFormatW( L"%d", err )
    };

    UtilReportEvent(
        err == JET_errSuccess ? eventInformation : eventError,
        GENERAL_CATEGORY,
        err == JET_errSuccess ? RBSREVERT_EXECUTE_SUCCESS_ID : RBSREVERT_EXECUTE_FAILED_ID,
        3,
        rgcwsz,
        0,
        NULL,
        m_pinst );

    if ( err < JET_errSuccess )
    {
        if ( m_pcprintfRevertTrace )
        {
            TraceFuncComplete( m_pcprintfRevertTrace, __FUNCTION__, err );
        }

        for ( IRBSDBRC irbsdbrc = 0; irbsdbrc <= m_irbsdbrcMaxInUse; ++irbsdbrc )
        {
            if ( m_rgprbsdbrcAttached[ irbsdbrc ]->CprintfIRSTrace() )
            {
                TraceFuncComplete( m_rgprbsdbrcAttached[ irbsdbrc ]->CprintfIRSTrace(), __FUNCTION__, JET_errSuccess );
            }
        }
    }
    else
    {
        (*m_pcprintfRevertTrace)( 
            "Successfully completed reverting the database files from %ws %ws to %ws %ws. Total pages reverted %lld.\r\n", 
            wszTimeFrom,
            wszDateFrom,
            wszTimeTo,
            wszDateTo,
            m_prbsrchk->rbsrchkfilehdr.le_cPagesReverted + le_cPagesRevertedCurRBSGen );
    }

    return err;
}
